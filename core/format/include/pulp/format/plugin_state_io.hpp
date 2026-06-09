#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace pulp::state {
class StateStore;
}

namespace pulp::format {
class Processor;

namespace plugin_state_io {

using EnvelopeMigrationFn =
    std::function<bool(std::span<const uint8_t> source,
                       std::vector<uint8_t>& destination)>;

uint32_t current_envelope_version();

enum class RestoreBlobFormat : uint8_t {
    unknown,
    legacy_state_store,
    envelope,
};

enum class RestoreDiagnosticStatus : uint8_t {
    ok,
    empty_blob,
    unknown_format,
    truncated_envelope,
    unsupported_envelope_version,
    envelope_migration_unavailable,
    payload_size_mismatch,
    crc_mismatch,
    empty_store_payload,
    total_size_budget_exceeded,
    store_size_budget_exceeded,
    plugin_size_budget_exceeded,
};

[[nodiscard]] const char* restore_diagnostic_status_name(
    RestoreDiagnosticStatus status) noexcept;

struct RestoreDiagnosticPolicy {
    uint64_t max_total_bytes = 0;
    uint64_t max_store_bytes = 0;
    uint64_t max_plugin_bytes = 0;
    uint64_t large_state_threshold_bytes = 0;
};

struct RestoreDiagnostics {
    RestoreDiagnosticStatus status = RestoreDiagnosticStatus::unknown_format;
    RestoreBlobFormat format = RestoreBlobFormat::unknown;
    uint64_t total_bytes = 0;
    uint32_t envelope_version = 0;
    uint64_t store_bytes = 0;
    uint64_t plugin_bytes = 0;
    bool requires_envelope_migration = false;
    bool requires_plugin_restore = false;
    bool may_require_resource_relink = false;
    bool large_state = false;

    [[nodiscard]] bool ok() const noexcept { return status == RestoreDiagnosticStatus::ok; }
};

/// Inspect a host-facing plugin state blob without mutating live plugin state.
/// This performs structural validation, size-budget checks, and large-state /
/// plugin-payload signaling so callers can decide whether restore and resource
/// relink work should be dispatched asynchronously.
[[nodiscard]] RestoreDiagnostics inspect_restore_blob(
    std::span<const uint8_t> bytes,
    const RestoreDiagnosticPolicy& policy = {}) noexcept;

bool register_envelope_migration(uint32_t from_version,
                                 uint32_t to_version,
                                 EnvelopeMigrationFn migration);

/// Serialize the host-facing plugin state blob for a Processor.
///
/// StateStore remains the parameter-only inner payload. When the processor
/// exposes a non-empty plugin-owned blob, this helper wraps both payloads in
/// a versioned outer envelope. When the plugin-owned blob is empty, the
/// legacy raw StateStore blob is returned unchanged.
std::vector<uint8_t> serialize(const state::StateStore& store,
                               const Processor& processor);

/// Restore a host-facing plugin state blob.
///
/// Accepts both legacy raw StateStore blobs and the combined envelope emitted
/// by serialize(). On failure, the live StateStore is rolled back to its
/// previous state before returning false.
bool deserialize(std::span<const uint8_t> bytes,
                 state::StateStore& store,
                 Processor& processor);

} // namespace plugin_state_io
} // namespace pulp::format
