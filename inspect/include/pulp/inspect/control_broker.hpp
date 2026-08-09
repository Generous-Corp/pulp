#pragma once

#include <pulp/inspect/control_admission.hpp>
#include <pulp/inspect/control_artifacts.hpp>

#include <memory>
#include <mutex>
#include <span>
#include <string_view>

namespace pulp::inspect {

struct ControlBrokerConfig {
    ControlIdentityRegistryConfig identities;
    ControlGrantStoreConfig grants;
    ControlAdmissionPolicy admission;
    std::optional<ControlOperationStoreConfig> operation_store;
    std::optional<ControlArtifactStoreConfig> artifact_store;
    /// Trusted wall-clock source. It may run inside broker authority
    /// coordination and must be bounded, non-blocking, and non-reentrant: it
    /// must not call this broker or any store owned by it.
    ControlOperationStore::WallClock wall_clock = [] { return std::chrono::system_clock::now(); };
    /// Broker-owned process liveness. The daemon supplies the OS-backed
    /// implementation; tests and unsupported compositions conservatively keep
    /// process-scoped principals until their reconnect lease expires.
    std::function<ControlProcessLiveness(const ControlPeerEvidence&)> process_liveness =
        [](const ControlPeerEvidence&) { return ControlProcessLiveness::Unknown; };
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

    /// The steady clock has the same trusted callback contract as
    /// ControlBrokerConfig::wall_clock. It may run inside broker coordination
    /// and must not call this broker or any of its owned stores.
    explicit ControlBroker(
        ControlBrokerConfig config = {}, std::shared_ptr<ControlSecurityAuditLog> audit_log = {},
        Clock clock = [] { return std::chrono::steady_clock::now(); });
    ~ControlBroker();
    ControlBroker(const ControlBroker&) = delete;
    ControlBroker& operator=(const ControlBroker&) = delete;

    const ControlBrokerId& broker_id() const;
    bool is_listening() const {
        return false;
    }

    ControlBootstrapResult issue_bootstrap(const VerifiedControlPeerIdentity& client_peer);
    ControlClientResult redeem_bootstrap(std::string_view ticket_id,
                                         std::span<const std::uint8_t> secret,
                                         const VerifiedControlPeerIdentity& client_peer,
                                         std::string_view durable_principal = {},
                                         ControlDurableClientLifetime durable_lifetime =
                                             ControlDurableClientLifetime::Broker);
    bool refresh_client(const ControlClientId& client_id,
                        const VerifiedControlPeerIdentity& client_peer);
    ControlBrokerLifecycleResult disconnect_client(const ControlClientId& client_id,
                                                   const VerifiedControlPeerIdentity& client_peer,
                                                   std::string_view decision_id);

    ControlRegistrationResult register_instance(const VerifiedControlPeerIdentity& host_peer,
                                                ControlRegistrationRequest request);
    bool heartbeat(const ControlRegistrationId& registration_id,
                   const VerifiedControlPeerIdentity& host_peer);
    ControlBrokerLifecycleResult unregister_instance(const ControlRegistrationId& registration_id,
                                                     const VerifiedControlPeerIdentity& host_peer,
                                                     std::string_view decision_id);

    ControlGrantResult issue_grant(const VerifiedControlPeerIdentity& client_peer,
                                   ControlGrantRequest request, ControlConsentDecision consent);
    ControlGrantStatus revoke_grant(const ControlGrantId& grant_id, std::string_view decision_id);
    bool is_granted(const ControlGrantId& grant_id, const ControlClientId& client_id,
                    const ControlRegistrationId& registration_id, InspectorCapability capability);

    /// Atomically resolves every permission term from broker-owned state.
    /// Artifact reads use their producer-lineage reauthorization path instead
    /// and can never be admitted by minting an ArtifactRead grant.
    ControlAdmissionResult admit_operation(const VerifiedControlPeerIdentity& client_peer,
                                           const ControlRequestEnvelope& request);
    /// Reauthorizes and resolves an existing idempotent receipt without
    /// creating a fresh admission.
    ControlAdmissionResult replay_operation(const VerifiedControlPeerIdentity& client_peer,
                                            const ControlRequestEnvelope& request);
    bool revalidate_operation(const VerifiedControlPeerIdentity& client_peer,
                              const ControlAdmissionPlan& plan);
    bool operation_store_ready() const;
    bool artifact_store_ready() const;
    ControlOperationStoreResult begin_operation(const VerifiedControlPeerIdentity& client_peer,
                                                const ControlAdmissionPlan& plan);
    ControlOperationStoreResult finish_operation(const VerifiedControlPeerIdentity& client_peer,
                                                 const ControlAdmissionPlan& plan,
                                                 ControlReceiptState terminal_state,
                                                 ControlOperationResult result = {});
    ControlOperationStoreResult cancel_operation(const VerifiedControlPeerIdentity& client_peer,
                                                 const ControlClientId& client_id,
                                                 std::string_view request_id,
                                                 std::string_view reason);
    bool operation_cancellation_requested(const VerifiedControlPeerIdentity& client_peer,
                                          const ControlAdmissionPlan& plan) const;
    ControlExecutionCheckpoint execution_checkpoint(const VerifiedControlPeerIdentity& client_peer,
                                                    const ControlAdmissionPlan& plan);
    std::optional<ControlOperationReceipt>
    operation_receipt(const ControlReceiptId& receipt_id) const;

    /// Publishes blob and immutable lineage metadata while the producer
    /// receipt is Running. finish_operation rejects every artifact handle that
    /// was not published through this store first.
    ControlArtifactStoreResult
    store_operation_artifact(const VerifiedControlPeerIdentity& client_peer,
                             const ControlAdmissionPlan& plan, std::span<const std::uint8_t> bytes,
                             ControlArtifactProperties properties);
    /// Broker-timestamped publication used by trusted operation adapters.
    ControlArtifactStoreResult
    store_operation_artifact(const VerifiedControlPeerIdentity& client_peer,
                             const ControlAdmissionPlan& plan, std::span<const std::uint8_t> bytes,
                             std::string content_type, ControlArtifactSensitivity sensitivity,
                             ControlArtifactRedactionState redaction_state,
                             std::chrono::milliseconds lifetime);
    std::size_t artifact_maximum_blob_bytes() const noexcept;

    /// Retrieves an artifact by opaque ID after broker-side reauthorization of
    /// its original producer grant, exact receipt lineage, and exact producing
    /// client. Peer identity alone is insufficient because one peer may own
    /// multiple client identities.
    ControlArtifactReadResult read_artifact(const VerifiedControlPeerIdentity& client_peer,
                                            const ControlClientId& requesting_client_id,
                                            std::string_view artifact_id, std::uint64_t offset,
                                            std::size_t maximum_bytes);

    std::optional<ControlClientIdentity> client(const ControlClientId& client_id) const;
    std::optional<ControlRegistration>
    registration(const ControlRegistrationId& registration_id) const;
    std::vector<ControlRegistration> registrations() const;
    std::optional<ControlGrant> grant(const ControlGrantId& grant_id);

    void sweep_expired();

  private:
    bool authorize_artifact_read_locked(const VerifiedControlPeerIdentity& client_peer,
                                        const ControlClientId& requesting_client_id,
                                        const ControlArtifactMetadata& metadata);
    ControlAdmissionResult admit_verified_operation(const VerifiedControlPeerIdentity& client_peer,
                                                    const ControlAdmissionRequest& request,
                                                    bool allow_new = true);
    bool revalidate_operation_locked(const VerifiedControlPeerIdentity& client_peer,
                                     const ControlAdmissionPlan& plan);
    void audit_denial(std::string_view action, const VerifiedControlPeerIdentity& observed_peer,
                      std::string_view reason, const ControlClientId& client_id = {},
                      const ControlRegistrationId& registration_id = {});
    bool owns_client_peer(const ControlClientId& client_id,
                          const VerifiedControlPeerIdentity& client_peer) const;
    bool owns_registration_peer(const ControlRegistrationId& registration_id,
                                const VerifiedControlPeerIdentity& host_peer) const;
    void request_active_cancellation(const ControlClientId& client_id,
                                     const ControlRegistrationId& registration_id,
                                     const ControlGrantId& grant_id, std::string_view reason);

    std::shared_ptr<ControlSecurityAuditLog> audit_log_;
    std::function<ControlProcessLiveness(const ControlPeerEvidence&)> process_liveness_;
    mutable std::mutex coordination_mutex_;
    ControlIdentityRegistry identities_;
    ControlGrantStore grants_;
    ControlAdmissionPolicy admission_policy_;
    ControlOperationStore::WallClock wall_clock_;
    std::unique_ptr<ControlOperationStore> operation_store_;
    std::unique_ptr<ControlArtifactStore> artifact_store_;
};

} // namespace pulp::inspect
