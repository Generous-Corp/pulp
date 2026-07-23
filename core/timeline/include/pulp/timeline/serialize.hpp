#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/schema_release.hpp>

#include <cstddef>
#include <cstdint>
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

struct ProjectSnapshotCounts {
    std::size_t assets = 0;
    std::size_t sequences = 0;
    std::size_t tracks = 0;
    std::size_t clips = 0;
    std::size_t notes = 0;
    std::size_t device_placements = 0;
    std::size_t automation_lanes = 0;
    std::size_t automation_points = 0;
    std::size_t take_lanes = 0;
    std::size_t takes = 0;
    std::size_t take_comp_segments = 0;
};

// Allocation-light metadata view for project browsers and admission checks.
// The scan validates structural envelopes and quotas but deliberately does not
// construct a Project or resolve references between items.
struct ProjectSnapshotSummary {
    std::uint32_t schema_version = 0;
    ItemId project_id;
    std::string name;
    std::uint64_t next_item_id = 0;
    ItemId root_sequence_id;
    ProjectSnapshotCounts counts;
};

runtime::Result<SerializedSnapshot, PersistenceError>
serialize_project(const Project& project, const SchemaRegistry& registry,
                  const SerializeOptions& options = {});

runtime::Result<SerializedSnapshot, PersistenceError>
serialize_project_for_release(const Project& project, const SchemaRegistry& registry,
                              const SchemaReleaseMap& release,
                              const SerializeOptions& options = {});

runtime::Result<ProjectSnapshotSummary, PersistenceError>
peek_project_summary(std::string_view json, const SchemaRegistry& registry,
                     const DecodeLimits& limits = {});

runtime::Result<Project, PersistenceError> deserialize_project(std::string_view json,
                                                               const SchemaRegistry& registry,
                                                               const DecodeLimits& limits = {});

} // namespace pulp::timeline
