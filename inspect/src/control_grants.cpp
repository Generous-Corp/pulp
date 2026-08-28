#include <pulp/inspect/control_grants.hpp>

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <deque>
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
           consent.authority == ControlConsentAuthority::BrokerUserPrompt ||
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
    struct OneShotAuthorization {
        std::string operation_identity;
        bool committed = false;
        std::unordered_set<std::uint64_t> provisional_tokens;
    };

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

    void retire_locked(std::string grant_id) {
        if (config.max_retired_grant_ids == 0)
            return;
        if (retired_grant_ids.contains(grant_id))
            return;
        const auto capacity = config.max_retired_grant_ids;
        if (retired_grant_order.size() == capacity) {
            retired_grant_ids.erase(retired_grant_order.front());
            retired_grant_order.pop_front();
        }
        retired_grant_order.push_back(grant_id);
        retired_grant_ids.emplace(std::move(grant_id));
    }

    ControlIdentityRegistry& identities;
    std::shared_ptr<ControlSecurityAuditLog> audit_log;
    ControlGrantStoreConfig config;
    Clock clock;
    std::mutex mutex;
    std::unordered_map<std::string, ControlGrant> grants;
    std::unordered_map<std::string, OneShotAuthorization> one_shot_authorizations;
    std::uint64_t next_authorization_token = 1;
    std::deque<std::string> retired_grant_order;
    std::unordered_set<std::string> retired_grant_ids;
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
    const auto now = impl_->clock();
    if (now >= client->expires_at) {
        result.status = ControlGrantStatus::ClientUnavailable;
        impl_->audit_denial(request, control_grant_status_id(result.status));
        return result;
    }
    if (now >= registration->expires_at) {
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
    if (!trusted_consent(consent) ||
        (consent.authority == ControlConsentAuthority::BrokerUserPrompt &&
         (!consent.expires_at || now >= *consent.expires_at))) {
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
    const bool interactive_consent =
        consent.authority == ControlConsentAuthority::TrustedPulpCli ||
        consent.authority == ControlConsentAuthority::TrustedHostUi ||
        consent.authority == ControlConsentAuthority::BrokerUserPrompt;
    // Decision replay and operation reuse are separate policies: every
    // interactive decision is consumed once, while only GPU-health reads spend
    // their grant on one fresh operation identity.
    const bool one_shot_gpu_health =
        interactive_consent &&
        std::ranges::find(request.capabilities,
                          InspectorCapability::GpuHealthRead) !=
            request.capabilities.end();
    if (std::ranges::find(request.capabilities, InspectorCapability::RuntimeEval) !=
            request.capabilities.end() &&
        !interactive_consent) {
        result.status = ControlGrantStatus::ConsentRequired;
        impl_->audit_denial(request, control_grant_status_id(result.status));
        return result;
    }
    ControlGrant grant{
        .grant_id = ControlGrantId{*grant_id},
        .broker_id = impl_->identities.broker_id(),
        .client_id = request.client_id,
        .registration_id = request.registration_id,
        .session_id = registration->session_id,
        .instance_id = registration->instance_id,
        .publication_id = registration->publication_id,
        .capabilities = std::move(request.capabilities),
        .consent_decision_id = std::move(consent.decision_id),
        .expires_at = now + ttl,
    };
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->grants.size() >= impl_->config.max_grants) {
            result.status = ControlGrantStatus::ResourceExhausted;
            impl_->audit_denial(request,
                                control_grant_status_id(result.status));
            return result;
        }
        if (interactive_consent &&
            impl_->consumed_consent_decisions.contains(
                grant.consent_decision_id)) {
            result.status = ControlGrantStatus::ConsentReplay;
            impl_->audit_denial(request,
                                control_grant_status_id(result.status));
            return result;
        }
        if (interactive_consent &&
            impl_->consumed_consent_decisions.size() >=
                impl_->config.max_consumed_consent_decisions) {
            result.status = ControlGrantStatus::ResourceExhausted;
            impl_->audit_denial(request,
                                control_grant_status_id(result.status));
            return result;
        }
        const bool retired_collision = impl_->retired_grant_ids.contains(
            grant.grant_id.value);
        const auto insertion_time = impl_->clock();
        if (consent.authority == ControlConsentAuthority::BrokerUserPrompt &&
            insertion_time >= *consent.expires_at) {
            result.status = ControlGrantStatus::ConsentRequired;
            impl_->audit_denial(request, control_grant_status_id(result.status));
            return result;
        }
        if (ttl > std::chrono::steady_clock::time_point::max() - insertion_time) {
            result.status = ControlGrantStatus::InvalidRequest;
            impl_->audit_denial(request, control_grant_status_id(result.status));
            return result;
        }
        grant.expires_at = insertion_time + ttl;
        const auto [_, inserted] = retired_collision
            ? std::pair{impl_->grants.end(), false}
            : impl_->grants.emplace(grant.grant_id.value, grant);
        if (!inserted) {
            result.status = ControlGrantStatus::ResourceExhausted;
            impl_->audit_denial(request,
                                control_grant_status_id(result.status));
            return result;
        }
        if (one_shot_gpu_health)
            impl_->one_shot_authorizations.emplace(grant.grant_id.value,
                                                   Impl::OneShotAuthorization{});
        if (interactive_consent)
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
        if (found == impl_->grants.end()) {
            return impl_->retired_grant_ids.contains(grant_id.value)
                       ? ControlGrantStatus::Revoked
                       : ControlGrantStatus::NotFound;
        }
        snapshot = found->second;
        impl_->one_shot_authorizations.erase(grant_id.value);
        impl_->grants.erase(found);
        impl_->retire_locked(grant_id.value);
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
        for (auto it = impl_->grants.begin(); it != impl_->grants.end();) {
            if (it->second.client_id == client_id) {
                revoked.push_back(it->second);
                impl_->retire_locked(it->second.grant_id.value);
                impl_->one_shot_authorizations.erase(it->second.grant_id.value);
                it = impl_->grants.erase(it);
            } else {
                ++it;
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
        for (auto it = impl_->grants.begin(); it != impl_->grants.end();) {
            if (it->second.registration_id == registration_id) {
                revoked.push_back(it->second);
                impl_->retire_locked(it->second.grant_id.value);
                impl_->one_shot_authorizations.erase(it->second.grant_id.value);
                it = impl_->grants.erase(it);
            } else {
                ++it;
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

std::optional<ControlOperationAuthorization> ControlGrantStore::authorize_operation(
    const ControlGrantId& grant_id, const ControlClientId& client_id,
    const ControlRegistrationId& registration_id, InspectorCapability capability,
    std::string_view operation_identity) {
    if (operation_identity.empty() ||
        !is_granted(grant_id, client_id, registration_id, capability))
        return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    const auto now = impl_->clock();
    const auto found = impl_->grants.find(grant_id.value);
    if (found == impl_->grants.end() || found->second.revoked ||
        now >= found->second.expires_at || found->second.client_id != client_id ||
        found->second.registration_id != registration_id ||
        std::find(found->second.capabilities.begin(), found->second.capabilities.end(),
                  capability) == found->second.capabilities.end())
        return std::nullopt;
    if (capability != InspectorCapability::GpuHealthRead)
        return ControlOperationAuthorization{
            .kind = ControlOperationAuthorizationKind::Reusable};
    const auto authorization = impl_->one_shot_authorizations.find(grant_id.value);
    if (authorization == impl_->one_shot_authorizations.end())
        return ControlOperationAuthorization{
            .kind = ControlOperationAuthorizationKind::Reusable};
    auto& state = authorization->second;
    if (state.committed)
        return ControlOperationAuthorization{
            .kind = ControlOperationAuthorizationKind::CommittedOneShot};
    if (state.operation_identity.empty())
        state.operation_identity = operation_identity;
    if (state.operation_identity != operation_identity)
        return std::nullopt;
    auto token = impl_->next_authorization_token++;
    if (token == 0)
        token = impl_->next_authorization_token++;
    if (token == 0 || !state.provisional_tokens.insert(token).second)
        return std::nullopt;
    return ControlOperationAuthorization{
        .kind = ControlOperationAuthorizationKind::ProvisionalOneShot,
        .reservation_token = token};
}

bool ControlGrantStore::commit_operation_authorization(
    const ControlGrantId& grant_id, std::string_view operation_identity,
    std::uint64_t reservation_token) {
    if (!grant_id || operation_identity.empty() || reservation_token == 0)
        return false;
    std::lock_guard lock(impl_->mutex);
    const auto authorization = impl_->one_shot_authorizations.find(grant_id.value);
    if (authorization == impl_->one_shot_authorizations.end() ||
        authorization->second.operation_identity != operation_identity)
        return false;
    auto& state = authorization->second;
    if (state.committed)
        return true;
    if (!state.provisional_tokens.contains(reservation_token))
        return false;
    state.committed = true;
    state.provisional_tokens.clear();
    return true;
}

bool ControlGrantStore::release_operation_authorization(
    const ControlGrantId& grant_id, std::string_view operation_identity,
    std::uint64_t reservation_token) {
    if (!grant_id || operation_identity.empty() || reservation_token == 0)
        return false;
    std::lock_guard lock(impl_->mutex);
    const auto authorization = impl_->one_shot_authorizations.find(grant_id.value);
    if (authorization == impl_->one_shot_authorizations.end() ||
        authorization->second.operation_identity != operation_identity ||
        authorization->second.committed)
        return false;
    auto& state = authorization->second;
    if (state.provisional_tokens.erase(reservation_token) == 0)
        return false;
    return true;
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

std::vector<ControlGrant> ControlGrantStore::sweep_expired() {
    const auto now = impl_->clock();
    std::vector<ControlGrant> candidates;
    {
        std::lock_guard lock(impl_->mutex);
        candidates.reserve(impl_->grants.size());
        for (const auto& [_, grant] : impl_->grants)
            candidates.push_back(grant);
    }

    std::vector<ControlGrantId> unavailable;
    unavailable.reserve(candidates.size());
    for (const auto& grant : candidates) {
        if (now < grant.expires_at &&
            (!impl_->identities.client(grant.client_id) ||
             !impl_->identities.registration(grant.registration_id))) {
            unavailable.push_back(grant.grant_id);
        }
    }

    std::vector<ControlGrant> expired;
    std::vector<ControlGrant> removed;
    {
        std::lock_guard lock(impl_->mutex);
        for (auto it = impl_->grants.begin(); it != impl_->grants.end();) {
            const bool identity_unavailable = std::ranges::any_of(
                unavailable, [&](const auto& grant_id) {
                    return grant_id == it->second.grant_id;
                });
            if (now < it->second.expires_at && !identity_unavailable) {
                ++it;
                continue;
            }
            if (now >= it->second.expires_at)
                expired.push_back(it->second);
            else
                removed.push_back(it->second);
            impl_->one_shot_authorizations.erase(it->second.grant_id.value);
            it = impl_->grants.erase(it);
        }
    }
    for (const auto& grant : expired) {
        impl_->audit(grant, "grant.expire", ControlSecurityOutcome::Expired,
                     "expired");
    }
    for (const auto& grant : removed) {
        impl_->audit(grant, "grant.remove-unavailable",
                     ControlSecurityOutcome::Revoked,
                     "identity-unavailable");
    }
    expired.insert(expired.end(), std::make_move_iterator(removed.begin()),
                   std::make_move_iterator(removed.end()));
    return expired;
}

} // namespace pulp::inspect
