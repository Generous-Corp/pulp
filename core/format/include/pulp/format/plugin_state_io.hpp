#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace pulp::state {
class StateStore;
}

namespace pulp::format {
class Processor;

namespace plugin_state_io {

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
