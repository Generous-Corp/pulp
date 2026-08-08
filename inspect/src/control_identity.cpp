#include <pulp/inspect/control_identity.hpp>

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace pulp::inspect {
namespace {

constexpr std::size_t kBootstrapSecretBytes = 32;

bool valid_peer_evidence(const ControlPeerEvidence& evidence) {
    return !evidence.user_id.empty() && evidence.process_id > 0 &&
           !evidence.process_start_id.empty() &&
           !evidence.executable_identity.empty() &&
           !evidence.publisher_id.empty();
}

std::string peer_fingerprint(const ControlPeerEvidence& evidence) {
    std::string canonical;
    canonical.reserve(evidence.user_id.size() +
                      evidence.process_start_id.size() +
                      evidence.executable_identity.size() +
                      evidence.publisher_id.size() + 48);
    canonical.append(std::to_string(static_cast<unsigned>(evidence.role)));
    canonical.push_back('\0');
    canonical.append(evidence.user_id);
    canonical.push_back('\0');
    canonical.append(std::to_string(evidence.process_id));
    canonical.push_back('\0');
    canonical.append(evidence.process_start_id);
    canonical.push_back('\0');
    canonical.append(evidence.executable_identity);
    canonical.push_back('\0');
    canonical.append(evidence.publisher_id);
    return pulp::runtime::sha256_hex(canonical);
}

std::optional<std::string> random_id(std::string_view prefix) {
    const auto bytes = pulp::runtime::secure_random_bytes(16);
    if (!bytes)
        return std::nullopt;
    return std::string(prefix) + pulp::runtime::hex_encode(*bytes);
}

template <typename Duration>
std::optional<std::chrono::steady_clock::time_point> expiry_after(
    std::chrono::steady_clock::time_point now, Duration ttl) {
    const auto duration =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(ttl);
    if (duration <= std::chrono::steady_clock::duration::zero() ||
        duration > std::chrono::steady_clock::time_point::max() - now) {
        return std::nullopt;
    }
    return now + duration;
}

bool valid_registration_request(const ControlRegistrationRequest& request) {
    if (request.session_id.empty() || request.instance_id.empty() ||
        request.publication_id.empty() || request.artifact_digest.size() != 64 ||
        request.manifest.capabilities.empty()) {
        return false;
    }
    if (!std::ranges::all_of(request.artifact_digest, [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) {
        return false;
    }
    if (!validate_control_manifest_detailed(request.manifest).valid ||
        request.manifest.profile == ControlBuildProfile::ProductionStripped) {
        return false;
    }
    for (const auto capability : request.manifest.capabilities) {
        if (!capability_is_grantable(capability)) {
            return false;
        }
        // Phase 4's first runtime slice is deliberately T0-only. Merely
        // declaring render.offline on a standalone or bridged host must not
        // expand that process's authority before those tiers land.
        if (capability == InspectorCapability::RenderOffline &&
            (request.host_tier != ControlHostTier::OfflineJob ||
             request.manifest.profile != ControlBuildProfile::TestDeterministic)) {
            return false;
        }
    }
    return true;
}

bool peer_can_register(ControlHostTier tier, ControlPeerRole role) {
    if (tier == ControlHostTier::OfflineJob)
        return role == ControlPeerRole::OfflineHost;
    if (tier == ControlHostTier::Standalone)
        return role == ControlPeerRole::StandaloneHost;
    return role == ControlPeerRole::TrustedHostBridge;
}

std::string exact_key(std::string_view session_id,
                      std::string_view instance_id,
                      std::string_view publication_id) {
    std::string key;
    key.reserve(session_id.size() + instance_id.size() +
                publication_id.size() + 3);
    key.append(session_id);
    key.push_back('\0');
    key.append(instance_id);
    key.push_back('\0');
    key.append(publication_id);
    return key;
}

} // namespace

ControlPeerVerifier::ControlPeerVerifier(Authority authority)
    : authority_(std::move(authority)) {}

std::optional<VerifiedControlPeerIdentity> ControlPeerVerifier::verify(
    ControlPeerEvidence evidence) const {
    if (!authority_ || !valid_peer_evidence(evidence) ||
        !authority_(evidence)) {
        return std::nullopt;
    }
    auto fingerprint = peer_fingerprint(evidence);
    return VerifiedControlPeerIdentity(
        std::move(evidence), std::move(fingerprint));
}

std::string_view control_identity_status_id(ControlIdentityStatus status) {
    switch (status) {
        case ControlIdentityStatus::Accepted: return "accepted";
        case ControlIdentityStatus::InvalidRequest: return "invalid-request";
        case ControlIdentityStatus::PeerRoleMismatch:
            return "peer-role-mismatch";
        case ControlIdentityStatus::HostUnavailable: return "host-unavailable";
        case ControlIdentityStatus::AttestationUnavailable:
            return "attestation-unavailable";
        case ControlIdentityStatus::IdentityMismatch:
            return "identity-mismatch";
        case ControlIdentityStatus::NotFound: return "not-found";
        case ControlIdentityStatus::Expired: return "expired";
        case ControlIdentityStatus::Replay: return "replay";
        case ControlIdentityStatus::ResourceExhausted:
            return "resource-exhausted";
        case ControlIdentityStatus::EntropyUnavailable:
            return "entropy-unavailable";
    }
    return "invalid-request";
}

ControlBootstrapSecret::ControlBootstrapSecret(
    std::span<const std::uint8_t> bytes)
    : bytes_(bytes.begin(), bytes.end()) {}

ControlBootstrapSecret::~ControlBootstrapSecret() {
    clear();
}

ControlBootstrapSecret::ControlBootstrapSecret(
    ControlBootstrapSecret&& other) noexcept
    : bytes_(std::move(other.bytes_)) {
    other.bytes_.clear();
}

ControlBootstrapSecret& ControlBootstrapSecret::operator=(
    ControlBootstrapSecret&& other) noexcept {
    if (this != &other) {
        clear();
        bytes_ = std::move(other.bytes_);
        other.bytes_.clear();
    }
    return *this;
}

void ControlBootstrapSecret::clear() noexcept {
    pulp::runtime::secure_zero_memory(bytes_.data(), bytes_.size());
    bytes_.clear();
}

class ControlIdentityRegistry::Impl {
public:
    struct BootstrapState {
        std::vector<std::uint8_t> secret;
        std::string peer_fingerprint;
        std::chrono::steady_clock::time_point expires_at;
    };

    Impl(ControlIdentityRegistryConfig config_in,
         std::shared_ptr<ControlSecurityAuditLog> audit_log_in,
         Clock clock_in)
        : config(std::move(config_in)),
          audit_log(std::move(audit_log_in)),
          clock(std::move(clock_in)) {
        if (const auto id = random_id("broker-"))
            broker_id.value = *id;
    }

    ~Impl() {
        for (auto& [_, ticket] : tickets)
            pulp::runtime::secure_zero_memory(
                ticket.secret.data(), ticket.secret.size());
    }

    void audit(ControlSecurityAuditEntry entry) {
        if (!audit_log)
            return;
        entry.broker_id = broker_id.value;
        audit_log->append(std::move(entry));
    }

    void sweep_locked(std::chrono::steady_clock::time_point now) {
        for (auto it = tickets.begin(); it != tickets.end();) {
            if (now < it->second.expires_at) {
                ++it;
                continue;
            }
            pulp::runtime::secure_zero_memory(
                it->second.secret.data(), it->second.secret.size());
            audit(ControlSecurityAuditEntry{
                .action = "bootstrap.expire",
                .peer_fingerprint = it->second.peer_fingerprint,
                .outcome = ControlSecurityOutcome::Expired,
                .reason = "expired",
            });
            it = tickets.erase(it);
        }
        for (auto it = clients.begin(); it != clients.end();) {
            if (now < it->second.expires_at)
                ++it;
            else {
                audit(ControlSecurityAuditEntry{
                    .action = "client.expire",
                    .peer_fingerprint = it->second.peer_fingerprint,
                    .client_id = it->second.client_id.value,
                    .outcome = ControlSecurityOutcome::Expired,
                    .reason = "expired",
                });
                it = clients.erase(it);
            }
        }
        for (auto it = registrations.begin(); it != registrations.end();) {
            if (now < it->second.expires_at) {
                ++it;
                continue;
            }
            exact_registrations.erase(exact_key(
                it->second.session_id, it->second.instance_id,
                it->second.publication_id));
            audit(ControlSecurityAuditEntry{
                .action = "registration.expire",
                .peer_fingerprint = it->second.peer_fingerprint,
                .registration_id = it->second.registration_id.value,
                .session_id = it->second.session_id,
                .instance_id = it->second.instance_id,
                .publication_id = it->second.publication_id,
                .outcome = ControlSecurityOutcome::Expired,
                .reason = "expired",
            });
            it = registrations.erase(it);
        }
    }

    ControlIdentityRegistryConfig config;
    std::shared_ptr<ControlSecurityAuditLog> audit_log;
    Clock clock;
    ControlBrokerId broker_id;
    mutable std::mutex mutex;
    std::unordered_map<std::string, BootstrapState> tickets;
    std::unordered_map<std::string, ControlClientIdentity> clients;
    std::unordered_map<std::string, ControlRegistration> registrations;
    std::unordered_map<std::string, std::string> exact_registrations;
};

ControlIdentityRegistry::ControlIdentityRegistry(
    ControlIdentityRegistryConfig config,
    std::shared_ptr<ControlSecurityAuditLog> audit_log,
    Clock clock)
    : impl_(std::make_unique<Impl>(
          std::move(config), std::move(audit_log), std::move(clock))) {}

ControlIdentityRegistry::~ControlIdentityRegistry() = default;

const ControlBrokerId& ControlIdentityRegistry::broker_id() const {
    return impl_->broker_id;
}

ControlBootstrapResult ControlIdentityRegistry::issue_bootstrap(
    const VerifiedControlPeerIdentity& expected_peer) {
    ControlBootstrapResult result;
    if (!impl_->broker_id ||
        expected_peer.evidence().role != ControlPeerRole::Client) {
        result.status = impl_->broker_id
                            ? ControlIdentityStatus::PeerRoleMismatch
                            : ControlIdentityStatus::EntropyUnavailable;
        impl_->audit(ControlSecurityAuditEntry{
            .action = "bootstrap.issue",
            .peer_fingerprint = std::string(expected_peer.fingerprint()),
            .outcome = ControlSecurityOutcome::Denied,
            .reason = std::string(control_identity_status_id(result.status)),
        });
        return result;
    }
    const auto now = impl_->clock();
    const auto expires = expiry_after(now, impl_->config.bootstrap_ttl);
    if (!expires) {
        result.status = ControlIdentityStatus::InvalidRequest;
        impl_->audit(ControlSecurityAuditEntry{
            .action = "bootstrap.issue",
            .peer_fingerprint = std::string(expected_peer.fingerprint()),
            .outcome = ControlSecurityOutcome::Denied,
            .reason = "invalid-request",
        });
        return result;
    }
    const auto ticket_id = random_id("bootstrap-");
    auto secret =
        pulp::runtime::secure_random_bytes(kBootstrapSecretBytes);
    if (!ticket_id || !secret) {
        result.status = ControlIdentityStatus::EntropyUnavailable;
        impl_->audit(ControlSecurityAuditEntry{
            .action = "bootstrap.issue",
            .peer_fingerprint = std::string(expected_peer.fingerprint()),
            .outcome = ControlSecurityOutcome::Denied,
            .reason = "entropy-unavailable",
        });
        return result;
    }
    ControlBootstrapSecret transient_secret(*secret);
    pulp::runtime::secure_zero_memory(secret->data(), secret->size());

    {
        std::lock_guard lock(impl_->mutex);
        impl_->sweep_locked(now);
        if (impl_->tickets.size() >= impl_->config.max_bootstrap_tickets) {
            result.status = ControlIdentityStatus::ResourceExhausted;
            impl_->audit(ControlSecurityAuditEntry{
                .action = "bootstrap.issue",
                .peer_fingerprint =
                    std::string(expected_peer.fingerprint()),
                .outcome = ControlSecurityOutcome::Denied,
                .reason = "resource-exhausted",
            });
            return result;
        }
        impl_->tickets.emplace(
            *ticket_id,
            Impl::BootstrapState{
                {transient_secret.bytes().begin(),
                 transient_secret.bytes().end()},
                std::string(expected_peer.fingerprint()), *expires});
    }

    result.status = ControlIdentityStatus::Accepted;
    result.ticket.emplace(ControlBootstrapTicket{
        *ticket_id, impl_->broker_id,
        ControlBootstrapSecret(transient_secret.bytes()), *expires});
    impl_->audit(ControlSecurityAuditEntry{
        .action = "bootstrap.issue",
        .peer_fingerprint = std::string(expected_peer.fingerprint()),
        .outcome = ControlSecurityOutcome::Accepted,
        .reason = "accepted",
    });
    return result;
}

ControlClientResult ControlIdentityRegistry::redeem_bootstrap(
    std::string_view ticket_id,
    std::span<const std::uint8_t> secret,
    const VerifiedControlPeerIdentity& observed_peer) {
    ControlClientResult result;
    const auto now = impl_->clock();
    Impl::BootstrapState ticket;
    {
        std::lock_guard lock(impl_->mutex);
        auto found = impl_->tickets.find(std::string(ticket_id));
        if (found == impl_->tickets.end()) {
            result.status = ControlIdentityStatus::Replay;
            impl_->audit(ControlSecurityAuditEntry{
                .action = "bootstrap.redeem",
                .peer_fingerprint =
                    std::string(observed_peer.fingerprint()),
                .outcome = ControlSecurityOutcome::Denied,
                .reason = "replay",
            });
            return result;
        }
        ticket = std::move(found->second);
        impl_->tickets.erase(found);
    }

    const bool expired = now >= ticket.expires_at;
    const bool peer_matches =
        ticket.peer_fingerprint == observed_peer.fingerprint();
    const bool secret_matches =
        secret.size() == ticket.secret.size() &&
        !secret.empty() &&
        pulp::runtime::constant_time_equal(
            secret.data(), ticket.secret.data(), secret.size());
    pulp::runtime::secure_zero_memory(
        ticket.secret.data(), ticket.secret.size());

    if (expired || !peer_matches || !secret_matches) {
        result.status = expired ? ControlIdentityStatus::Expired
                                : ControlIdentityStatus::IdentityMismatch;
        impl_->audit(ControlSecurityAuditEntry{
            .action = "bootstrap.redeem",
            .peer_fingerprint = std::string(observed_peer.fingerprint()),
            .outcome = ControlSecurityOutcome::Denied,
            .reason = std::string(control_identity_status_id(result.status)),
        });
        return result;
    }

    const auto client_id = random_id("client-");
    const auto expires = expiry_after(now, impl_->config.client_ttl);
    if (!client_id || !expires) {
        result.status = client_id ? ControlIdentityStatus::InvalidRequest
                                  : ControlIdentityStatus::EntropyUnavailable;
        impl_->audit(ControlSecurityAuditEntry{
            .action = "bootstrap.redeem",
            .peer_fingerprint =
                std::string(observed_peer.fingerprint()),
            .outcome = ControlSecurityOutcome::Denied,
            .reason = std::string(control_identity_status_id(result.status)),
        });
        return result;
    }
    ControlClientIdentity client{
        ControlClientId{*client_id}, impl_->broker_id,
        std::string(observed_peer.fingerprint()), *expires};
    {
        std::lock_guard lock(impl_->mutex);
        impl_->sweep_locked(now);
        if (impl_->clients.size() >= impl_->config.max_clients) {
            result.status = ControlIdentityStatus::ResourceExhausted;
            impl_->audit(ControlSecurityAuditEntry{
                .action = "bootstrap.redeem",
                .peer_fingerprint =
                    std::string(observed_peer.fingerprint()),
                .outcome = ControlSecurityOutcome::Denied,
                .reason = "resource-exhausted",
            });
            return result;
        }
        impl_->clients.emplace(client.client_id.value, client);
    }
    result.status = ControlIdentityStatus::Accepted;
    result.client = client;
    impl_->audit(ControlSecurityAuditEntry{
        .action = "bootstrap.redeem",
        .peer_fingerprint = std::string(observed_peer.fingerprint()),
        .client_id = client.client_id.value,
        .outcome = ControlSecurityOutcome::Accepted,
        .reason = "accepted",
    });
    return result;
}

bool ControlIdentityRegistry::refresh_client(
    const ControlClientId& client_id,
    const VerifiedControlPeerIdentity& peer) {
    const auto now = impl_->clock();
    const auto expires = expiry_after(now, impl_->config.client_ttl);
    if (!expires) {
        impl_->audit(ControlSecurityAuditEntry{
            .action = "client.refresh",
            .peer_fingerprint = std::string(peer.fingerprint()),
            .client_id = client_id.value,
            .outcome = ControlSecurityOutcome::Denied,
            .reason = "invalid-request",
        });
        return false;
    }
    bool refreshed = false;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->sweep_locked(now);
        auto found = impl_->clients.find(client_id.value);
        if (found != impl_->clients.end() &&
            found->second.peer_fingerprint == peer.fingerprint()) {
            found->second.expires_at = *expires;
            refreshed = true;
        }
    }
    impl_->audit(ControlSecurityAuditEntry{
        .action = "client.refresh",
        .peer_fingerprint = std::string(peer.fingerprint()),
        .client_id = client_id.value,
        .outcome = refreshed ? ControlSecurityOutcome::Accepted
                             : ControlSecurityOutcome::Denied,
        .reason = refreshed ? "accepted" : "identity-mismatch",
    });
    if (!refreshed)
        return false;
    return true;
}

bool ControlIdentityRegistry::disconnect_client(
    const ControlClientId& client_id) {
    std::optional<ControlClientIdentity> disconnected;
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->clients.find(client_id.value);
        if (found != impl_->clients.end()) {
            disconnected = found->second;
            impl_->clients.erase(found);
        }
    }
    impl_->audit(ControlSecurityAuditEntry{
        .action = "client.disconnect",
        .peer_fingerprint = disconnected
                                ? disconnected->peer_fingerprint
                                : std::string{},
        .client_id = client_id.value,
        .outcome = disconnected ? ControlSecurityOutcome::Revoked
                                : ControlSecurityOutcome::Denied,
        .reason = disconnected ? "disconnected" : "not-found",
    });
    return disconnected.has_value();
}

std::optional<ControlClientIdentity> ControlIdentityRegistry::client(
    const ControlClientId& client_id) const {
    const auto now = impl_->clock();
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->clients.find(client_id.value);
    if (found == impl_->clients.end() || now >= found->second.expires_at)
        return std::nullopt;
    return found->second;
}

ControlRegistrationResult ControlIdentityRegistry::register_instance(
    const VerifiedControlPeerIdentity& peer,
    ControlRegistrationRequest request) {
    ControlRegistrationResult result;
    const auto audit_denial = [&](ControlIdentityStatus status) {
        impl_->audit(ControlSecurityAuditEntry{
            .action = "registration.create",
            .peer_fingerprint = std::string(peer.fingerprint()),
            .session_id = request.session_id,
            .instance_id = request.instance_id,
            .publication_id = request.publication_id,
            .outcome = ControlSecurityOutcome::Denied,
            .reason = std::string(control_identity_status_id(status)),
        });
    };
    if (!impl_->broker_id) {
        result.status = ControlIdentityStatus::EntropyUnavailable;
        audit_denial(result.status);
        return result;
    }
    if (!valid_registration_request(request)) {
        result.status = ControlIdentityStatus::InvalidRequest;
        audit_denial(result.status);
        return result;
    }
    if (!peer_can_register(request.host_tier, peer.evidence().role)) {
        result.status = ControlIdentityStatus::PeerRoleMismatch;
        audit_denial(result.status);
        return result;
    }
    if (request.host_tier == ControlHostTier::SharedPluginHost) {
        result.status = ControlIdentityStatus::HostUnavailable;
        audit_denial(result.status);
        return result;
    }
    const auto now = impl_->clock();
    const auto expires = expiry_after(now, impl_->config.registration_ttl);
    const auto registration_id = random_id("registration-");
    if (!expires || !registration_id) {
        result.status = registration_id
                            ? ControlIdentityStatus::InvalidRequest
                            : ControlIdentityStatus::EntropyUnavailable;
        audit_denial(result.status);
        return result;
    }

    ControlRegistration registration{
        ControlRegistrationId{*registration_id},
        impl_->broker_id,
        request.host_tier,
        std::move(request.session_id),
        std::move(request.instance_id),
        std::move(request.publication_id),
        request.manifest.bundle_id,
        peer.evidence().publisher_id,
        control_manifest_digest(request.manifest),
        std::move(request.artifact_digest),
        {},
        request.manifest.profile,
        std::move(request.manifest.capabilities),
        std::string(peer.fingerprint()),
        *expires,
    };
    registration.consent_identity = control_consent_identity(
        registration.manifest_digest, registration.artifact_digest);
    const auto key = exact_key(registration.session_id,
                               registration.instance_id,
                               registration.publication_id);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->sweep_locked(now);
        if (impl_->registrations.size() >= impl_->config.max_registrations) {
            result.status = ControlIdentityStatus::ResourceExhausted;
            audit_denial(result.status);
            return result;
        }
        if (impl_->exact_registrations.contains(key)) {
            result.status = ControlIdentityStatus::IdentityMismatch;
            audit_denial(result.status);
            return result;
        }
        impl_->registrations.emplace(
            registration.registration_id.value, registration);
        impl_->exact_registrations.emplace(
            key, registration.registration_id.value);
    }
    result.status = ControlIdentityStatus::Accepted;
    result.registration = registration;
    impl_->audit(ControlSecurityAuditEntry{
        .action = "registration.create",
        .peer_fingerprint = registration.peer_fingerprint,
        .registration_id = registration.registration_id.value,
        .session_id = registration.session_id,
        .instance_id = registration.instance_id,
        .publication_id = registration.publication_id,
        .outcome = ControlSecurityOutcome::Accepted,
        .reason = "accepted",
    });
    return result;
}

bool ControlIdentityRegistry::heartbeat(
    const ControlRegistrationId& registration_id,
    const VerifiedControlPeerIdentity& peer) {
    const auto now = impl_->clock();
    const auto expires = expiry_after(now, impl_->config.registration_ttl);
    if (!expires) {
        impl_->audit(ControlSecurityAuditEntry{
            .action = "registration.heartbeat",
            .peer_fingerprint = std::string(peer.fingerprint()),
            .registration_id = registration_id.value,
            .outcome = ControlSecurityOutcome::Denied,
            .reason = "invalid-request",
        });
        return false;
    }
    bool refreshed = false;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->sweep_locked(now);
        auto found = impl_->registrations.find(registration_id.value);
        if (found != impl_->registrations.end() &&
            found->second.peer_fingerprint == peer.fingerprint()) {
            found->second.expires_at = *expires;
            refreshed = true;
        }
    }
    impl_->audit(ControlSecurityAuditEntry{
        .action = "registration.heartbeat",
        .peer_fingerprint = std::string(peer.fingerprint()),
        .registration_id = registration_id.value,
        .outcome = refreshed ? ControlSecurityOutcome::Accepted
                             : ControlSecurityOutcome::Denied,
        .reason = refreshed ? "accepted" : "identity-mismatch",
    });
    if (!refreshed)
        return false;
    return true;
}

bool ControlIdentityRegistry::unregister_instance(
    const ControlRegistrationId& registration_id,
    const VerifiedControlPeerIdentity& peer) {
    std::optional<ControlRegistration> removed;
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->registrations.find(registration_id.value);
        if (found != impl_->registrations.end() &&
            found->second.peer_fingerprint == peer.fingerprint()) {
            removed = found->second;
            impl_->exact_registrations.erase(exact_key(
                found->second.session_id, found->second.instance_id,
                found->second.publication_id));
            impl_->registrations.erase(found);
        }
    }
    impl_->audit(ControlSecurityAuditEntry{
        .action = "registration.unregister",
        .peer_fingerprint = std::string(peer.fingerprint()),
        .registration_id = registration_id.value,
        .session_id = removed ? removed->session_id : std::string{},
        .instance_id = removed ? removed->instance_id : std::string{},
        .publication_id = removed ? removed->publication_id : std::string{},
        .outcome = removed ? ControlSecurityOutcome::Revoked
                           : ControlSecurityOutcome::Denied,
        .reason = removed ? "unregistered" : "identity-mismatch",
    });
    return removed.has_value();
}

std::optional<ControlRegistration> ControlIdentityRegistry::registration(
    std::string_view session_id,
    std::string_view instance_id,
    std::string_view publication_id) const {
    if (session_id.empty() || instance_id.empty() || publication_id.empty())
        return std::nullopt;
    const auto now = impl_->clock();
    std::lock_guard lock(impl_->mutex);
    const auto key = exact_key(session_id, instance_id, publication_id);
    const auto exact = impl_->exact_registrations.find(key);
    if (exact == impl_->exact_registrations.end())
        return std::nullopt;
    const auto found = impl_->registrations.find(exact->second);
    if (found == impl_->registrations.end() ||
        now >= found->second.expires_at) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<ControlRegistration> ControlIdentityRegistry::registration(
    const ControlRegistrationId& registration_id) const {
    const auto now = impl_->clock();
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->registrations.find(registration_id.value);
    if (found == impl_->registrations.end() ||
        now >= found->second.expires_at) {
        return std::nullopt;
    }
    return found->second;
}

std::vector<ControlRegistration> ControlIdentityRegistry::registrations() const {
    const auto now = impl_->clock();
    std::lock_guard lock(impl_->mutex);
    std::vector<ControlRegistration> result;
    result.reserve(impl_->registrations.size());
    for (const auto& [_, registration] : impl_->registrations) {
        if (registration.expires_at > now)
            result.push_back(registration);
    }
    std::ranges::sort(result, {}, [](const auto& value) { return value.registration_id.value; });
    return result;
}

void ControlIdentityRegistry::sweep_expired() {
    std::lock_guard lock(impl_->mutex);
    impl_->sweep_locked(impl_->clock());
}

} // namespace pulp::inspect
