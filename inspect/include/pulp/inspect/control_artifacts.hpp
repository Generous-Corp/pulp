#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::inspect {

class ControlBroker;

struct ControlArtifactLineage {
    std::string broker_id;
    std::string receipt_id;
    std::string producer_client_id;
    std::string producer_registration_id;
    std::string session_id;
    std::string instance_id;
    std::string publication_id;
    std::string producer_capability_id;
    std::string producer_operation_id;
    std::uint32_t producer_operation_version = 0;
    std::string original_grant_id;
    std::string consent_decision_id;
    std::string manifest_digest;
    std::string producer_artifact_digest;
};

enum class ControlArtifactSensitivity : std::uint8_t {
    Public,
    Internal,
    Sensitive,
    Restricted,
};

enum class ControlArtifactDeletionState : std::uint8_t {
    Active,
    Deleted,
};

enum class ControlArtifactRedactionState : std::uint8_t {
    Original,
    Redacted,
};

struct ControlArtifactProperties {
    std::string content_type;
    std::uint64_t created_at_unix_ms = 0;
    std::uint64_t expires_at_unix_ms = 0;
    ControlArtifactSensitivity sensitivity = ControlArtifactSensitivity::Sensitive;
    ControlArtifactRedactionState redaction_state = ControlArtifactRedactionState::Original;
};

struct ControlArtifactMetadata {
    std::string artifact_id;
    ControlArtifactLineage lineage;
    std::string sha256;
    std::uint64_t byte_size = 0;
    std::string content_type;
    std::uint64_t created_at_unix_ms = 0;
    std::uint64_t expires_at_unix_ms = 0;
    ControlArtifactSensitivity sensitivity = ControlArtifactSensitivity::Sensitive;
    ControlArtifactDeletionState deletion_state = ControlArtifactDeletionState::Active;
    ControlArtifactRedactionState redaction_state = ControlArtifactRedactionState::Original;
};

enum class ControlArtifactStatus : std::uint8_t {
    Stored,
    Read,
    InvalidRequest,
    Unauthorized,
    NotFound,
    Corrupt,
    ResourceExhausted,
    IoError,
};

std::string_view control_artifact_status_id(ControlArtifactStatus status);

struct ControlArtifactStoreConfig {
    std::filesystem::path root;
    std::size_t maximum_blob_bytes = 16u * 1024u * 1024u;
    std::size_t maximum_chunk_bytes = 1u * 1024u * 1024u;
    std::chrono::milliseconds maximum_lifetime = std::chrono::hours{24};
};

struct ControlArtifactStoreResult {
    ControlArtifactStatus status = ControlArtifactStatus::InvalidRequest;
    std::optional<ControlArtifactMetadata> metadata;
};

struct ControlArtifactReadResult {
    ControlArtifactStatus status = ControlArtifactStatus::InvalidRequest;
    std::optional<ControlArtifactMetadata> metadata;
    std::vector<std::uint8_t> bytes;
    bool eof = false;
    std::string explanation;
};

/// Broker-owned content-addressed artifact persistence.
///
/// Blobs are durably published before their opaque ACL metadata. Two artifacts
/// with distinct lineage may therefore share immutable blob bytes without
/// sharing authority. Metadata is the only discoverable artifact publication;
/// an orphan blob left by a crash grants no access.
class ControlArtifactStore {
  public:
    using WallClock = std::function<std::chrono::system_clock::time_point()>;

    explicit ControlArtifactStore(
        ControlArtifactStoreConfig config,
        WallClock clock = [] { return std::chrono::system_clock::now(); });
    ~ControlArtifactStore();
    ControlArtifactStore(const ControlArtifactStore&) = delete;
    ControlArtifactStore& operator=(const ControlArtifactStore&) = delete;

    bool is_ready() const;

    ControlArtifactStoreResult store(std::span<const std::uint8_t> bytes,
                                     ControlArtifactLineage lineage,
                                     ControlArtifactProperties properties);

    /// Returns immutable publication metadata without reading blob bytes.
    /// Possession of metadata does not authorize a blob read; byte retrieval is
    /// a private broker-only capability that revalidates exact receipt lineage.
    std::optional<ControlArtifactMetadata> metadata(std::string_view artifact_id) const;

  private:
    friend class ControlBroker;
    ControlArtifactReadResult
    read_authorized(std::string_view artifact_id, std::uint64_t offset, std::size_t maximum_bytes,
                    const ControlArtifactMetadata& authorized_metadata) const;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
