#pragma once

#include <pulp/inspect/control_grants.hpp>
#include <pulp/inspect/control_protocol.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::inspect {

inline constexpr std::uint32_t kControlOperationReceiptSchemaVersion = 1;
inline constexpr std::string_view kControlOperationReceiptSchemaId =
    "dev.pulp.control/operation-receipt@1";
inline constexpr std::uint32_t kControlAuthorityBindingVersion = 1;

struct ControlReceiptId {
    std::string value;
    explicit operator bool() const {
        return !value.empty();
    }
    friend bool operator==(const ControlReceiptId&, const ControlReceiptId&) = default;
};

/// Canonical immutable snapshot of the authority under which an operation was
/// admitted. Admission plans and durable operation bindings carry this exact
/// value so authorization checks compare one model rather than projections.
struct ControlAuthorityBinding {
    ControlBrokerId broker_id;
    std::string client_principal;
    ControlClientId client_id;
    ControlRegistrationId registration_id;
    ControlGrantId grant_id;
    std::string session_id;
    std::string instance_id;
    std::string publication_id;
    std::string instance_generation;
    InspectorCapability capability = InspectorCapability::SessionDescribe;
    std::string operation_id;
    std::uint32_t operation_version = 1;
    std::string consent_decision_id;
    std::string manifest_digest;
    std::string producer_artifact_digest;
    std::int64_t deadline_unix_ms = 0;
    std::uint64_t expected_state_generation = 0;

    friend bool operator==(const ControlAuthorityBinding&,
                           const ControlAuthorityBinding&) = default;
};

/// The complete idempotency scope for one admitted operation. The caller must
/// hash the schema-validated canonical request bytes, not the received JSON
/// spelling, into `canonical_request_hash`.
struct ControlOperationBinding : ControlAuthorityBinding {
    std::string request_id;
    std::string idempotency_key;
    std::string canonical_request_hash;

    const ControlAuthorityBinding& authority_binding() const {
        return *this;
    }
};

struct ControlOperationResult {
    std::optional<ControlResultCode> result_code;
    ControlRetryClassification retry = ControlRetryClassification::Never;
    std::string explanation;
    std::string detail_json = "{}";
    std::string cancellation_reason;
    std::vector<ControlArtifactHandle> artifacts;
    std::vector<std::string> evidence_ids;
};

struct ControlOperationReceipt {
    std::uint32_t schema_version = kControlOperationReceiptSchemaVersion;
    ControlReceiptId receipt_id;
    ControlReceiptState state = ControlReceiptState::Admitted;
    bool cancellation_requested = false;
    std::string cancellation_reason;
    ControlOperationBinding binding;
    std::string binding_hash;
    ControlOperationResult result;
    std::int64_t created_at_unix_ms = 0;
    std::int64_t updated_at_unix_ms = 0;
};

enum class ControlOperationStoreStatus : std::uint8_t {
    Opened,
    Admitted,
    Replay,
    ReplayWindowExpired,
    Transitioned,
    CancellationRequested,
    IdempotencyConflict,
    RequestIdConflict,
    InvalidRequest,
    InvalidTransition,
    NotFound,
    ResourceExhausted,
    StoreUnavailable,
    MalformedStore,
    PersistenceError,
};

std::string_view control_operation_store_status_id(ControlOperationStoreStatus status);

struct ControlOperationStoreResult {
    ControlOperationStoreStatus status = ControlOperationStoreStatus::StoreUnavailable;
    std::optional<ControlOperationReceipt> receipt;
    std::string error;

    bool succeeded() const {
        return status == ControlOperationStoreStatus::Opened ||
               status == ControlOperationStoreStatus::Admitted ||
               status == ControlOperationStoreStatus::Replay ||
               status == ControlOperationStoreStatus::Transitioned ||
               status == ControlOperationStoreStatus::CancellationRequested;
    }
};

struct ControlOperationStoreConfig {
    std::filesystem::path directory;
    std::size_t max_receipts = 4096;
    std::size_t max_receipt_bytes = kControlMaximumEnvelopeBytes;
    std::size_t max_active_receipts_per_client = 64;
    /// Quota for one exact registration generation and instance identity.
    std::size_t max_active_receipts_per_registration_instance = 16;
    std::chrono::milliseconds replay_window = std::chrono::hours{24};
    std::chrono::milliseconds retention = std::chrono::hours{24 * 7};
};

/// Durable broker operation receipt and idempotency ledger.
///
/// The store owns its mutex and performs no callbacks while holding it. Broker
/// identity/grant coordination must therefore happen outside this class; no
/// broker lock needs to span filesystem I/O. `open()` must succeed before any
/// other operation. It fails closed on malformed committed metadata and turns
/// receipts left Running by a prior process into UnknownNeedsRefresh before
/// accepting new admissions.
class ControlOperationStore {
  public:
    using WallClock = std::function<std::chrono::system_clock::time_point()>;

    explicit ControlOperationStore(
        ControlOperationStoreConfig config,
        WallClock clock = [] { return std::chrono::system_clock::now(); });
    ~ControlOperationStore();
    ControlOperationStore(const ControlOperationStore&) = delete;
    ControlOperationStore& operator=(const ControlOperationStore&) = delete;

    ControlOperationStoreResult open();
    bool is_open() const;

    ControlOperationStoreResult admit(ControlOperationBinding binding);
    /// Atomically claims an admitted receipt for execution. Cancellation and
    /// the receipt's absolute deadline are resolved durably before Running can
    /// become visible.
    ControlOperationStoreResult begin(const ControlReceiptId& receipt_id);
    ControlOperationStoreResult transition(const ControlReceiptId& receipt_id,
                                           ControlReceiptState expected, ControlReceiptState next,
                                           ControlOperationResult result = {});
    ControlOperationStoreResult request_cancellation(const ControlReceiptId& receipt_id,
                                                     std::string reason);

    std::optional<ControlOperationReceipt> receipt(const ControlReceiptId& receipt_id) const;
    std::vector<ControlOperationReceipt> receipts() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
