#pragma once

#include <pulp/timeline/schema_registry.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace pulp::timeline {

/** @addtogroup timeline_persistence
 * @{
 */

/// Target version for one schema identity in a release projection.
struct SchemaVersionTarget {
    SchemaDomain domain = SchemaDomain::Document;
    std::string_view type_name;
    std::uint32_t version = 0;
};

/// Immutable set of schema-version targets identified by a release label.
///
/// The label and version span refer to static storage for built-in maps.
struct SchemaReleaseMap {
    std::string_view release_label;
    std::span<const SchemaVersionTarget> versions;

    /// Finds a target by domain and type name.
    ///
    /// @return A pointer into versions, valid for the map lifetime, or nullptr.
    const SchemaVersionTarget* find(SchemaDomain domain, std::string_view type_name) const noexcept;
};

/// Returns every built-in release projection in stable static storage.
std::span<const SchemaReleaseMap> builtin_timeline_schema_releases() noexcept;
/// Finds a built-in release projection by exact label.
///
/// @return A pointer with static lifetime, or nullptr when the label is unknown.
const SchemaReleaseMap*
find_builtin_timeline_schema_release(std::string_view release_label) noexcept;

/// @}

} // namespace pulp::timeline
