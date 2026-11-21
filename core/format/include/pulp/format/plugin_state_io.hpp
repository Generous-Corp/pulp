#pragma once

#include <cstdint>
#include <functional>
#include <memory>
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

/// CRC-32 (IEEE 802.3, reflected, init and final xor 0xFFFFFFFF) used by the
/// host state envelope and the AU compact state. Table driven, eight bytes per
/// step; the value is identical to the bitwise definition.
[[nodiscard]] uint32_t crc32(std::span<const uint8_t> bytes) noexcept;

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

/// The processor-owned payload a host blob carries: the published snapshot when
/// the processor has one, else a fresh serialize_plugin_state(). Never null; an
/// empty vector means the processor has no payload of its own. A published
/// snapshot is shared, not copied.
std::shared_ptr<const std::vector<uint8_t>> plugin_state_payload(const Processor& processor);

/// Restore from an already-split parameter payload and plugin payload.
///
/// @p store_blob is a StateStore::serialize() blob. This is the second half of
/// deserialize(): the same schema probe, listener-silent restore, and rollback
/// of both layers when either is rejected. Adapters that carry the two payloads
/// in their own container call this instead of re-wrapping them in an envelope.
bool deserialize_parts(std::span<const uint8_t> store_blob, std::span<const uint8_t> plugin_blob,
                       state::StateStore& store, Processor& processor);

} // namespace plugin_state_io
} // namespace pulp::format
