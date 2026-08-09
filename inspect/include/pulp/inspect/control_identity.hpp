#pragma once

#include <pulp/inspect/audit.hpp>
#include <pulp/inspect/capabilities.hpp>
#include <pulp/inspect/control_manifest.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::inspect {

struct ControlBrokerId {
    std::string value;
    explicit operator bool() const {
        return !value.empty();
    }
    friend bool operator==(const ControlBrokerId&, const ControlBrokerId&) = default;
};

struct ControlClientId {
    std::string value;
    explicit operator bool() const {
        return !value.empty();
    }
    friend bool operator==(const ControlClientId&, const ControlClientId&) = default;
};

struct ControlRegistrationId {
    std::string value;
    explicit operator bool() const {
        return !value.empty();
    }
    friend bool operator==(const ControlRegistrationId&, const ControlRegistrationId&) = default;
};

enum class ControlPeerRole : std::uint8_t {
    Client,
    OfflineHost,
    StandaloneHost,
    TrustedHostBridge,
};

/// Evidence observed by the local IPC carrier. Values supplied by a plugin or
/// client payload are not peer evidence and must never be passed here.
struct ControlPeerEvidence {
    ControlPeerRole role = ControlPeerRole::Client;
    std::string user_id;
    std::int64_t process_id = 0;
    std::string process_start_id;
    std::string executable_identity;
    std::string publisher_id;
};

/// Copyable proof minted only by the broker's trusted OS peer verifier.
class VerifiedControlPeerIdentity {
  public:
    const ControlPeerEvidence& evidence() const {
        return evidence_;
    }
    std::string_view fingerprint() const {
        return fingerprint_;
    }

    friend bool operator==(const VerifiedControlPeerIdentity& left,
                           const VerifiedControlPeerIdentity& right) {
        return left.fingerprint_ == right.fingerprint_;
    }

  private:
    friend class ControlPeerVerifier;
    VerifiedControlPeerIdentity(ControlPeerEvidence evidence, std::string fingerprint)
        : evidence_(std::move(evidence)), fingerprint_(std::move(fingerprint)) {}

    ControlPeerEvidence evidence_;
    std::string fingerprint_;
};

/// Converts carrier-observed process evidence into an unforgeable-by-payload
/// identity. The authority callback performs the platform-specific UID/SID,
/// process-generation, and executable-signing checks.
class ControlPeerVerifier {
  public:
    using Authority = std::function<bool(const ControlPeerEvidence&)>;

    explicit ControlPeerVerifier(Authority authority);
    std::optional<VerifiedControlPeerIdentity> verify(ControlPeerEvidence evidence) const;

  private:
    Authority authority_;
};

enum class ControlHostTier : std::uint8_t {
    OfflineJob,
    Standalone,
    SharedPluginHost,
};

std::string_view control_host_tier_id(ControlHostTier tier);

enum class ControlIdentityStatus : std::uint8_t {
    Accepted,
    InvalidRequest,
    PeerRoleMismatch,
    HostUnavailable,
    AttestationUnavailable,
    IdentityMismatch,
    NotFound,
    Expired,
    Replay,
    ResourceExhausted,
    EntropyUnavailable,
};

std::string_view control_identity_status_id(ControlIdentityStatus status);

/// Move-only bootstrap material. Destruction wipes the secret bytes.
class ControlBootstrapSecret {
  public:
    explicit ControlBootstrapSecret(std::span<const std::uint8_t> bytes);
    ~ControlBootstrapSecret();
    ControlBootstrapSecret(const ControlBootstrapSecret&) = delete;
    ControlBootstrapSecret& operator=(const ControlBootstrapSecret&) = delete;
    ControlBootstrapSecret(ControlBootstrapSecret&& other) noexcept;
    ControlBootstrapSecret& operator=(ControlBootstrapSecret&& other) noexcept;

    std::span<const std::uint8_t> bytes() const {
        return bytes_;
    }

  private:
    void clear() noexcept;
    std::vector<std::uint8_t> bytes_;
};

struct ControlBootstrapTicket {
    std::string ticket_id;
    ControlBrokerId broker_id;
    ControlBootstrapSecret secret;
    std::chrono::steady_clock::time_point expires_at;
};

struct ControlBootstrapResult {
    ControlIdentityStatus status = ControlIdentityStatus::InvalidRequest;
    std::optional<ControlBootstrapTicket> ticket;
};

struct ControlClientIdentity {
    ControlClientId client_id;
    ControlBrokerId broker_id;
    std::string peer_fingerprint;
    std::chrono::steady_clock::time_point expires_at;
    /// Broker-owned stable principal for a re-authenticating installed client.
    /// Empty identities remain strictly connection-scoped.
    std::string durable_principal;
};

struct ControlClientResult {
    ControlIdentityStatus status = ControlIdentityStatus::InvalidRequest;
    std::optional<ControlClientIdentity> client;
};

enum class ControlDurableClientLifetime : std::uint8_t {
    Broker,
    Process,
};

enum class ControlProcessLiveness : std::uint8_t {
    Unknown,
    Alive,
    Dead,
};

struct ControlRegistrationRequest {
    ControlHostTier host_tier = ControlHostTier::Standalone;
    std::string session_id;
    std::string instance_id;
    std::string publication_id;
    ControlManifest manifest;
    std::string artifact_digest;
};

struct ControlRegistration {
    ControlRegistrationId registration_id;
    ControlBrokerId broker_id;
    ControlHostTier host_tier = ControlHostTier::Standalone;
    std::string session_id;
    std::string instance_id;
    std::string publication_id;
    std::string plugin_id;
    std::string publisher_id;
    std::string manifest_digest;
    std::string artifact_digest;
    std::string consent_identity;
    ControlBuildProfile profile = ControlBuildProfile::ProductionStripped;
    std::vector<InspectorCapability> capabilities;
    std::string peer_fingerprint;
    std::chrono::steady_clock::time_point expires_at;
    /// Manifest build identity retained for exact instance/status reads.
    std::string build_id;
    /// Starts at one and advances only after an authenticated heartbeat.
    std::uint64_t liveness_generation = 1;
};

struct ControlRegistrationResult {
    ControlIdentityStatus status = ControlIdentityStatus::InvalidRequest;
    std::optional<ControlRegistration> registration;
};

struct ControlIdentityRegistryConfig {
    std::size_t max_registrations = 64;
    std::size_t max_bootstrap_tickets = 64;
    std::size_t max_clients = 16;
    std::chrono::milliseconds registration_ttl = std::chrono::seconds(30);
    std::chrono::milliseconds bootstrap_ttl = std::chrono::seconds(5);
    std::chrono::milliseconds client_ttl = std::chrono::minutes(5);
};

/// Broker-owned identity state. It contains no listener and accepts only
/// verified peers supplied by the future OS-authenticated IPC composition root.
class ControlIdentityRegistry {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    explicit ControlIdentityRegistry(
        ControlIdentityRegistryConfig config = {},
        std::shared_ptr<ControlSecurityAuditLog> audit_log = {},
        Clock clock = [] { return std::chrono::steady_clock::now(); });
    ~ControlIdentityRegistry();
    ControlIdentityRegistry(const ControlIdentityRegistry&) = delete;
    ControlIdentityRegistry& operator=(const ControlIdentityRegistry&) = delete;

    const ControlBrokerId& broker_id() const;

    ControlBootstrapResult issue_bootstrap(
        const VerifiedControlPeerIdentity& expected_peer);
    ControlClientResult redeem_bootstrap(
        std::string_view ticket_id,
        std::span<const std::uint8_t> secret,
        const VerifiedControlPeerIdentity& observed_peer,
        std::string_view durable_principal = {},
        ControlDurableClientLifetime durable_lifetime =
            ControlDurableClientLifetime::Broker);
    /// Removes process-scoped clients whose kernel process is gone or whose
    /// reconnect lease expired. The broker owns grant/cancellation cleanup for
    /// every returned identity.
    std::vector<ControlClientId> reclaim_process_clients(
        const std::function<ControlProcessLiveness(const ControlPeerEvidence&)>&
            process_liveness);
    bool refresh_client(const ControlClientId& client_id,
                        const VerifiedControlPeerIdentity& peer);
    bool disconnect_client(const ControlClientId& client_id);
    std::optional<ControlClientIdentity> client(const ControlClientId& client_id) const;

    ControlRegistrationResult register_instance(const VerifiedControlPeerIdentity& peer,
                                                ControlRegistrationRequest request);
    bool heartbeat(const ControlRegistrationId& registration_id,
                   const VerifiedControlPeerIdentity& peer);
    bool unregister_instance(const ControlRegistrationId& registration_id,
                             const VerifiedControlPeerIdentity& peer);
    std::optional<ControlRegistration> registration(
        std::string_view session_id,
        std::string_view instance_id,
        std::string_view publication_id) const;
    std::optional<ControlRegistration> registration(
        const ControlRegistrationId& registration_id) const;
    /// Snapshot of every currently-live exact registration. Human labels are
    /// metadata only; callers must select by instance_id and reject ambiguity.
    std::vector<ControlRegistration> registrations() const;

    void sweep_expired();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
