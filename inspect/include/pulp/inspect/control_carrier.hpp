#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace pulp::inspect {

/// Filesystem identity captured for an owner-private local socket endpoint.
/// A daemon may retain this across its liveness probe so cleanup refuses an
/// endpoint that changed before final revalidation.
struct ControlEndpointIdentity {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;

    friend bool operator==(const ControlEndpointIdentity&,
                           const ControlEndpointIdentity&) = default;
};

/// Deterministic owner-specific directory for the control carrier.
std::filesystem::path default_control_runtime_directory();

/// Deterministic control directory below an explicitly supplied runtime root.
/// This overload is intended for embedding and deterministic tests; preparation
/// still rejects relative or non-private paths.
std::filesystem::path default_control_runtime_directory(const std::filesystem::path& runtime_root);

/// Deterministic LocalSocket endpoint for the control broker.
std::filesystem::path default_control_endpoint_path();
std::filesystem::path default_control_endpoint_path(const std::filesystem::path& runtime_root);

/// Create or validate an absolute owner-only runtime directory. Existing
/// symlinks, non-directories, group/other access, and extended ACLs fail closed.
/// Platforms without the verified local carrier return false.
bool prepare_control_runtime_directory(const std::filesystem::path& runtime_directory);

/// Capture a socket only when its parent is private and its path is an
/// owner-owned 0600 local endpoint.
std::optional<ControlEndpointIdentity>
control_endpoint_identity(const std::filesystem::path& endpoint_path);

/// Remove an endpoint only if its device and inode still match the identity
/// captured before the caller's stale/liveness decision.
bool remove_stale_control_endpoint(const std::filesystem::path& endpoint_path,
                                   const ControlEndpointIdentity& expected_identity);

/// Convenience for cleanup performed while the caller already holds the
/// per-user daemon singleton and has established that the endpoint is stale.
bool remove_stale_control_endpoint(const std::filesystem::path& endpoint_path);

} // namespace pulp::inspect
