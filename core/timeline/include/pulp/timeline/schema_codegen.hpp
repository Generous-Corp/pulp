#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timeline/schema_json.hpp>
#include <pulp/timeline/schema_registry.hpp>

#include <cstddef>
#include <string>

namespace pulp::timeline {

/** @addtogroup timeline_persistence
 * @{
 */

/// Emits a deterministic JSON-Schema projection of a SchemaRegistry.
///
/// Every TypeSchema becomes a `$defs` entry containing its domain, current
/// version, fields, required set, and migration edges. The result is canonical
/// JSON, so equivalent registries produce byte-identical output independent of
/// registration order.
///
/// This manifest is the authoritative input for the Timeline JavaScript,
/// TypeScript, MCP, and CLI projections; those surfaces derive from the schema
/// contract instead of maintaining independent type descriptions.
///
/// @param registry Immutable source registry.
/// @param maximum_bytes Hard ceiling for the generated document.
/// @return The canonical manifest, or OutputLimitExceeded when the projection
///         cannot fit within maximum_bytes.
runtime::Result<std::string, PersistenceError>
emit_schema_manifest(const SchemaRegistry& registry, std::size_t maximum_bytes = 8ull * 1024ull *
                                                                                 1024ull);

/// Manifest-shape version consumed by Timeline surface generators.
inline constexpr std::uint32_t schema_manifest_version = 1;

/// @}

} // namespace pulp::timeline
