#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_registry.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace pulp::timeline {

struct SerializeOptions {
    std::size_t max_output_bytes = 1024ull * 1024ull * 1024ull;
};

struct SerializedSnapshot {
    std::string json;
    bool has_opaque_objects = false;
};

runtime::Result<SerializedSnapshot, PersistenceError>
serialize_project(const Project& project, const SchemaRegistry& registry,
                  const SerializeOptions& options = {});

runtime::Result<Project, PersistenceError>
deserialize_project(std::string_view json, const SchemaRegistry& registry,
                    const DecodeLimits& limits = {});

} // namespace pulp::timeline
