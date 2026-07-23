#pragma once

#include <pulp/timeline/schema_registry.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace pulp::timeline {

struct SchemaVersionTarget {
    SchemaDomain domain = SchemaDomain::Document;
    std::string_view type_name;
    std::uint32_t version = 0;
};

struct SchemaReleaseMap {
    std::string_view release_label;
    std::span<const SchemaVersionTarget> versions;

    const SchemaVersionTarget* find(SchemaDomain domain, std::string_view type_name) const noexcept;
};

std::span<const SchemaReleaseMap> builtin_timeline_schema_releases() noexcept;
const SchemaReleaseMap*
find_builtin_timeline_schema_release(std::string_view release_label) noexcept;

} // namespace pulp::timeline
