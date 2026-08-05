#include <pulp/inspect/control_grants.hpp>

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace pulp::inspect {
namespace {

std::optional<std::string> random_grant_id() {
    const auto bytes = pulp::runtime::secure_random_bytes(16);
    if (!bytes)
        return std::nullopt;
    return "grant-" + pulp::runtime::hex_encode(*bytes);
}

bool trusted_consent(const ControlConsentDecision& consent) {
    if (!consent.approved || consent.decision_id.empty())
        return false;
    return consent.authority == ControlConsentAuthority::TrustedPulpCli ||
           consent.authority == ControlConsentAuthority::TrustedHostUi ||
           consent.authority == ControlConsentAuthority::ExistingUserPolicy;
}

bool valid_capabilities(std::span<const InspectorCapability> requested,
                        std::span<const InspectorCapability> available) {
    if (requested.empty())
        return false;
    std::unordered_set<unsigned> seen;
    for (const auto capability : requested) {
        if (capability == InspectorCapability::Unavailable ||
            !capability_is_grantable(capability) ||
            !seen.insert(static_cast<unsigned>(capability)).second ||
            std::find(available.begin(), available.end(), capability) ==
                available.end()) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string_view control_grant_status_id(ControlGrantStatus status) {
    switch (status) {
        case ControlGrantStatus::Granted: return "granted";
        case ControlGrantStatus::InvalidRequest: return "invalid-request";
        case ControlGrantStatus::BrokerMismatch: return "broker-mismatch";
        case ControlGrantStatus::ClientUnavailable:
            return "client-unavailable";
        case ControlGrantStatus::RegistrationUnavailable:
            return "registration-unavailable";
        case ControlGrantStatus::ConsentRequired: return "consent-required";
        case ControlGrantStatus::ConsentReplay: return "consent-replay";
        case ControlGrantStatus::CapabilityUnavailable:
            return "capability-unavailable";
        case ControlGrantStatus::ResourceExhausted:
            return "resource-exhausted";
        case ControlGrantStatus::NotFound: return "not-found";
        case ControlGrantStatus::Expired: return "expired";
        case ControlGrantStatus::Revoked: return "revoked";
    }
    return "invalid-request";
}

class ControlGrantStore::Impl {
public:
    Impl(ControlIdentityRegistry& identities_in,
         std::shared_ptr<ControlSecurityAuditLog> audit_log_in,
         ControlGrantStoreConfig config_in,
         Clock clock_in)
        : identities(identities_in),
          audit_log(std::move(audit_log_in)),
          config(std::move(config_in)),
          clock(std::move(clock_in)) {}

    void audit(const ControlGrant& grant,
               std::string_view action,
               ControlSecurityOutcome outcome,
               std::string_view reason,
               InspectorCapability capability =
                   InspectorCapability::Unavailable) {
        if (!audit_log)
            return;
        audit_log->append(ControlSecurityAuditEntry{
            .action = std::string(action),
            .broker_id = grant.broker_id.value,
            .client_id = grant.client_id.value,
            .registration_id = grant.registration_id.value,
            .grant_id = grant.grant_id.value,
            .session_id = grant.session_id,
            .instance_id = grant.instance_id,
            .publication_id = grant.publication_id,
            .capability_id = capability == InspectorCapability::Unavailable
                                 ? std::string{}
                                 : std::string(pulp::inspect::capability_id(
                                       capability)),
            .outcome = outcome,
            .reason = std::string(reason),
        });
    }

    void audit_denial(const ControlGrantRequest& request,
                      std::string_view reason) {
        if (!audit_log)
            return;
        audit_log->append(ControlSecurityAuditEntry{
            .action = "grant.issue",
            .broker_id = identities.broker_id().value,
            .client_id = request.client_id.value,
            .registration_id = request.registration_id.value,
            .outcome = ControlSecurityOutcome::Denied,
            .reason = std::string(reason),
        });
    }

    ControlIdentityRegistry& identities;
    std::shared_ptr<ControlSecurityAuditLog> audit_log;
    ControlGrantStoreConfig config;
    Clock clock;
    std::mutex mutex;
    std::unordered_map<std::string, ControlGrant> grants;
    std::unordered_set<std::string> consumed_consent_decisions;
};

ControlGrantStore::ControlGrantStore(
    ControlIdentityRegistry& identities,
    std::shared_ptr<ControlSecurityAuditLog> audit_log,
    ControlGrantStoreConfig config,
    Clock clock)
    : impl_(std::make_unique<Impl>(
          identities, std::move(audit_log), std::move(config),
          std::move(clock))) {}

ControlGrantStore::~ControlGrantStore() = default;

ControlGrantResult ControlGrantStore::issue(
    ControlGrantRequest request,
    ControlConsentDecision consent) {
    ControlGrantResult result;
    const auto client = impl_->identities.client(request.client_id);
    if (!client) {
        result.status = ControlGrantStatus::ClientUnavailable;
        impl_->audit_denial(request, control_grant_status_id(result.status));
        return result;
    }
    const auto registration =
        impl_->identities.registration(request.registration_id);
    if (!registration) {
        result.status = ControlGrantStatus::RegistrationUnavailable;
        impl_->audit_denial(request, control_grant_status_id(result.status));
        return result;
    }
    if (client->broker_id != impl_->identities.broker_id() ||
        registration->broker_id != impl_->identities.broker_id()) {
        result.status = ControlGrantStatus::BrokerMismatch;
        impl_->audit_denial(request, control_grant_status_id(result.status));
        return result;
    }
    if (!trusted_consent(consent)) {
        result.status = ControlGrantStatus::ConsentRequired;
        impl_->audit_denial(request, control_grant_status_id(result.status));
        return result;
    }
    if (!valid_capabilities(request.capabilities,
                            registration->capabilities)) {
        result.status = ControlGrantStatus::CapabilityUnavailable;
        impl_->audit_denial(request, control_grant_status_id(result.status));
        return result;
    }
    if (request.ttl <= std::chrono::milliseconds::zero() ||
        request.ttl > impl_->config.maximum_ttl) {
        result.status = ControlGrantStatus::InvalidRequest;
        impl_->audit_denial(request, control_grant_status_id(result.status));
        return result;
    }
    const auto now = impl_->clock();
    const auto ttl = std::chrono::duration_cast<
        std::chrono::steady_clock::duration>(request.ttl);
    if (ttl > std::chrono::steady_clock::time_point::max() - now) {
        result.status = ControlGrantStatus::InvalidRequest;
        impl_->audit_denial(request, control_grant_status_id(result.status));
        return result;
    }
    const auto grant_id = random_grant_id();
    if (!grant_id) {
        result.status = ControlGrantStatus::ResourceExhausted;
        impl_->audit_denial(request, control_grant_status_id(result.status));
        return result;
    }
    const bool one_shot_consent =
        consent.authority == ControlConsentAuthority::TrustedPulpCli ||
        consent.authority == ControlConsentAuthority::TrustedHostUi;
    ControlGrant grant{
        ControlGrantId{*grant_id},
        impl_->identities.broker_id(),
        request.client_id,
        request.registration_id,
        registration->session_id,
        registration->instance_id,
        registration->publication_id,
        std::move(request.capabilities),
        std::move(consent.decision_id),
        now + ttl,
        false,
    };
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->grants.size() >= impl_->config.max_grants) {
            result.status = ControlGrantStatus::ResourceExhausted;
            impl_->audit_denial(request,
                                control_grant_status_id(result.status));
            return result;
        }
        if (one_shot_consent &&
            impl_->consumed_consent_decisions.contains(
                grant.consent_decision_id)) {
            result.status = ControlGrantStatus::ConsentReplay;
            impl_->audit_denial(request,
                                control_grant_status_id(result.status));
            return result;
        }
        if (one_shot_consent &&
            impl_->consumed_consent_decisions.size() >=
                impl_->config.max_consumed_consent_decisions) {
            result.status = ControlGrantStatus::ResourceExhausted;
            impl_->audit_denial(request,
                                control_grant_status_id(result.status));
            return result;
        }
        const auto [_, inserted] =
            impl_->grants.emplace(grant.grant_id.value, grant);
        if (!inserted) {
            result.status = ControlGrantStatus::ResourceExhausted;
            impl_->audit_denial(request,
                                control_grant_status_id(result.status));
            return result;
        }
        if (one_shot_consent)
            impl_->consumed_consent_decisions.emplace(
                grant.consent_decision_id);
    }
    result.status = ControlGrantStatus::Granted;
    result.grant = grant;
    for (const auto capability : grant.capabilities) {
        impl_->audit(grant, "grant.issue", ControlSecurityOutcome::Accepted,
                     "granted", capability);
    }
    return result;
}

ControlGrantStatus ControlGrantStore::revoke(
    const ControlGrantId& grant_id,
    std::string_view decision_id) {
    if (!grant_id || decision_id.empty())
        return ControlGrantStatus::InvalidRequest;
    ControlGrant snapshot;
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->grants.find(grant_id.value);
        if (found == impl_->grants.end())
            return ControlGrantStatus::NotFound;
        if (found->second.revoked)
            return ControlGrantStatus::Revoked;
        found->second.revoked = true;
        snapshot = found->second;
    }
    impl_->audit(snapshot, "grant.revoke", ControlSecurityOutcome::Revoked,
                 "revoked");
    return ControlGrantStatus::Revoked;
}

std::size_t ControlGrantStore::revoke_client(
    const ControlClientId& client_id,
    std::string_view decision_id) {
    if (!client_id || decision_id.empty())
        return 0;
    std::vector<ControlGrant> revoked;
    {
        std::lock_guard lock(impl_->mutex);
        for (auto& [_, grant] : impl_->grants) {
            if (grant.client_id == client_id && !grant.revoked) {
                grant.revoked = true;
                revoked.push_back(grant);
            }
        }
    }
    for (const auto& grant : revoked) {
        impl_->audit(grant, "grant.revoke-client",
                     ControlSecurityOutcome::Revoked, "revoked");
    }
    return revoked.size();
}

std::size_t ControlGrantStore::revoke_registration(
    const ControlRegistrationId& registration_id,
    std::string_view decision_id) {
    if (!registration_id || decision_id.empty())
        return 0;
    std::vector<ControlGrant> revoked;
    {
        std::lock_guard lock(impl_->mutex);
        for (auto& [_, grant] : impl_->grants) {
            if (grant.registration_id == registration_id && !grant.revoked) {
                grant.revoked = true;
                revoked.push_back(grant);
            }
        }
    }
    for (const auto& grant : revoked) {
        impl_->audit(grant, "grant.revoke-registration",
                     ControlSecurityOutcome::Revoked, "revoked");
    }
    return revoked.size();
}

bool ControlGrantStore::is_granted(
    const ControlGrantId& grant_id,
    const ControlClientId& client_id,
    const ControlRegistrationId& registration_id,
    InspectorCapability capability) {
    const auto now = impl_->clock();
    ControlGrant snapshot;
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->grants.find(grant_id.value);
        if (found == impl_->grants.end() || found->second.revoked ||
            now >= found->second.expires_at) {
            return false;
        }
        snapshot = found->second;
    }
    if (snapshot.client_id != client_id ||
        snapshot.registration_id != registration_id ||
        std::find(snapshot.capabilities.begin(), snapshot.capabilities.end(),
                  capability) == snapshot.capabilities.end()) {
        return false;
    }
    const auto client = impl_->identities.client(client_id);
    const auto registration =
        impl_->identities.registration(registration_id);
    return client && registration &&
           client->broker_id == snapshot.broker_id &&
           registration->broker_id == snapshot.broker_id &&
           registration->session_id == snapshot.session_id &&
           registration->instance_id == snapshot.instance_id &&
           registration->publication_id == snapshot.publication_id;
}

std::optional<ControlGrant> ControlGrantStore::grant(
    const ControlGrantId& grant_id) {
    const auto now = impl_->clock();
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->grants.find(grant_id.value);
    if (found == impl_->grants.end() || found->second.revoked ||
        now >= found->second.expires_at) {
        return std::nullopt;
    }
    return found->second;
}

void ControlGrantStore::sweep_expired() {
    const auto now = impl_->clock();
    std::vector<ControlGrant> expired;
    {
        std::lock_guard lock(impl_->mutex);
        for (auto it = impl_->grants.begin(); it != impl_->grants.end();) {
            if (now < it->second.expires_at) {
                ++it;
                continue;
            }
            expired.push_back(it->second);
            it = impl_->grants.erase(it);
        }
    }
    for (const auto& grant : expired) {
        impl_->audit(grant, "grant.expire", ControlSecurityOutcome::Expired,
                     "expired");
    }
}

} // namespace pulp::inspect
