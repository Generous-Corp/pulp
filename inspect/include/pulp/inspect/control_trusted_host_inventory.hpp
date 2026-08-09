#pragma once

#include <pulp/inspect/control_identity.hpp>

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

enum class ControlTrustedHostInventoryStatus {
    Prepared,
    PlatformUnavailable,
    InvalidRequest,
    UnsupportedArtifact,
    UnsafePath,
    SnapshotFailed,
    ManifestInvalid,
    ArtifactInvalid,
    SignatureInvalid,
    ResourceExhausted,
    EntropyUnavailable,
};

std::string_view control_trusted_host_inventory_status_id(ControlTrustedHostInventoryStatus status);

struct ControlTrustedHostLaunchIntent {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
    ControlHostTier host_tier = ControlHostTier::Standalone;
    friend bool operator==(const ControlTrustedHostLaunchIntent&,
                           const ControlTrustedHostLaunchIntent&) = default;
};

struct ControlTrustedHostInventoryConfig {
    std::filesystem::path staging_root;
    std::uint64_t broker_generation = 0;
    std::size_t maximum_entries = 64;
    std::size_t maximum_executable_bytes = 512u * 1024u * 1024u;
    std::size_t maximum_manifest_bytes = 1024u * 1024u;
    std::chrono::milliseconds ttl = std::chrono::seconds(10);
};

struct ControlTrustedHostInventoryTicket {
    std::string inventory_id;
    std::chrono::steady_clock::time_point expires_at;
};

struct ControlTrustedHostInventoryPrepareResult {
    ControlTrustedHostInventoryStatus status = ControlTrustedHostInventoryStatus::InvalidRequest;
    std::optional<ControlTrustedHostInventoryTicket> ticket;
};

/// Exact static-code expectation derived from the staged destination. This is
/// consistency evidence, not a publisher trust decision; broker-owned source
/// selection remains mandatory and this API grants no authority.
struct ControlTrustedHostStaticExpectation {
    std::string executable_identity;
    std::string publisher_id;
};

struct ControlTrustedHostRuntimeDependencyPolicy {
    std::string filename;
    std::string digest;
    ControlTrustedHostStaticExpectation static_expectation;
};

struct ControlTrustedHostPreparationPolicy {
    std::string executable_digest;
    std::string manifest_digest;
    ControlTrustedHostStaticExpectation static_expectation;
    std::vector<ControlTrustedHostRuntimeDependencyPolicy> runtime_dependencies;
    std::uint64_t working_directory_device = 0;
    std::uint64_t working_directory_inode = 0;
};

/// Immutable broker-owned launch material. The backing directory is removed
/// when the last snapshot owner releases it; no admission credential is stored
/// in that directory or encoded in its path.
class ControlTrustedHostSnapshot {
  public:
    ControlTrustedHostSnapshot(ControlTrustedHostSnapshot&&) noexcept;
    ControlTrustedHostSnapshot& operator=(ControlTrustedHostSnapshot&&) noexcept;
    ~ControlTrustedHostSnapshot();
    ControlTrustedHostSnapshot(const ControlTrustedHostSnapshot&) = delete;
    ControlTrustedHostSnapshot& operator=(const ControlTrustedHostSnapshot&) = delete;

    const std::filesystem::path& executable() const;
    const std::vector<std::string>& arguments() const;
    const std::filesystem::path& working_directory() const;
    bool working_directory_matches_policy() const;
    int working_directory_descriptor() const;
    const ControlRegistrationRequest& registration() const;
    const ControlTrustedHostStaticExpectation& static_expectation() const;
    std::uint64_t broker_generation() const;
    std::chrono::steady_clock::time_point expires_at() const;

  private:
    struct Impl;
    explicit ControlTrustedHostSnapshot(std::unique_ptr<Impl> impl);
    friend class ControlTrustedHostInventory;
    std::unique_ptr<Impl> impl_;
};

/// Process-local snapshot store intended to be owned by the broker composition
/// root. Tickets and broker-minted registration fields are recognized only by
/// this object; constructing another inventory cannot admit or register a host
/// with the broker and grants no cross-process authority.
class ControlTrustedHostInventory {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    explicit ControlTrustedHostInventory(
        ControlTrustedHostInventoryConfig config,
        Clock clock = [] { return std::chrono::steady_clock::now(); });
    ~ControlTrustedHostInventory();
    ControlTrustedHostInventory(const ControlTrustedHostInventory&) = delete;
    ControlTrustedHostInventory& operator=(const ControlTrustedHostInventory&) = delete;

    /// Raw snapshot preparation is available on macOS v1. Other platforms
    /// retain this API as a fail-closed seam for a future native verifier.
    ControlTrustedHostInventoryPrepareResult prepare(
        const ControlTrustedHostLaunchIntent& intent,
        std::optional<ControlTrustedHostPreparationPolicy> policy = std::nullopt);
    std::optional<ControlTrustedHostSnapshot> consume(std::string_view inventory_id);
    std::size_t sweep();
    std::size_t size() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
