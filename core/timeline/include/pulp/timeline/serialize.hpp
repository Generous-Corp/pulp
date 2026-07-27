#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timeline/command.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/schema_release.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pulp::timeline {

/** @addtogroup timeline_persistence
 * @{
 */

/// Output quota for project serialization.
struct SerializeOptions {
    /// Maximum bytes accepted in the returned JSON document.
    std::size_t max_output_bytes = 1024ull * 1024ull * 1024ull;
};

/// Canonical serialized project plus extension-preservation status.
///
/// has_opaque_objects is true when the serialized Project contains
/// OpaqueContent. Each opaque envelope is emitted from the validated raw JSON
/// retained by that model value; registered typed content does not set the flag.
struct SerializedSnapshot {
    /// Complete serialized project document.
    std::string json;
    /// Whether at least one OpaqueContent envelope was emitted.
    bool has_opaque_objects = false;
};

/// Structural item counts extracted from a serialized project.
struct ProjectSnapshotCounts {
    /// Number of media assets.
    std::size_t assets = 0;
    /// Number of sequences.
    std::size_t sequences = 0;
    /// Number of tracks across all sequences.
    std::size_t tracks = 0;
    /// Number of clips across all tracks.
    std::size_t clips = 0;
    /// Number of note events across note-content clips.
    std::size_t notes = 0;
    /// Number of device placements across all tracks.
    std::size_t device_placements = 0;
    /// Number of automation lanes across all tracks.
    std::size_t automation_lanes = 0;
    /// Number of automation points across all lanes.
    std::size_t automation_points = 0;
    /// Number of take lanes across all tracks.
    std::size_t take_lanes = 0;
    /// Number of takes across all take lanes.
    std::size_t takes = 0;
    /// Number of selected comp segments across all take lanes.
    std::size_t take_comp_segments = 0;
    /// Number of sequence markers.
    std::size_t markers = 0;
    /// Number of sequence regions.
    std::size_t regions = 0;
    /// Number of launch scenes.
    std::size_t scenes = 0;
    /// Number of launch slots across all scenes.
    std::size_t slots = 0;
    /// Number of chord/scale context events.
    std::size_t chord_scale_events = 0;
    /// Number of groove-template steps.
    std::size_t groove_steps = 0;
};

/// Allocation-light metadata view for project browsers and admission checks.
///
/// The scan validates structural envelopes and quotas but deliberately does
/// not construct a Project or resolve references between items.
struct ProjectSnapshotSummary {
    /// Version of the outer project schema envelope.
    std::uint32_t schema_version = 0;
    /// Project identity.
    ItemId project_id;
    /// Authored project name.
    std::string name;
    /// Next never-used project ItemId value.
    std::uint64_t next_item_id = 0;
    /// Root arrangement sequence identity.
    ItemId root_sequence_id;
    /// Structurally counted item totals.
    ProjectSnapshotCounts counts;
};

/// Serializes a model-valid project as canonical UTF-8 JSON.
///
/// Registered codecs are resolved through registry. Output is deterministic
/// and bounded by options.max_output_bytes.
///
/// @param project Immutable source project.
/// @param registry Schema contracts and extension codecs used by the project.
/// @param options Output quota.
/// @return Canonical JSON, or a path-bearing error for invalid UTF-8, registry
///         incompatibility, codec failure, or output quota exhaustion.
runtime::Result<SerializedSnapshot, PersistenceError>
serialize_project(const Project& project, const SchemaRegistry& registry,
                  const SerializeOptions& options = {});

/// Serializes a project using the schema versions pinned by a release map.
///
/// The current canonical snapshot is rewritten through registered downgrade
/// migrations. The release map must cover compatible registered structural
/// types; unsupported content or missing migration paths fail closed.
///
/// @param project Immutable source project.
/// @param registry Schema contracts and migrations.
/// @param release Target versions for the release projection.
/// @param options Output quota applied to current and rewritten documents.
/// @return Canonical release JSON or a structured persistence error.
runtime::Result<SerializedSnapshot, PersistenceError>
serialize_project_for_release(const Project& project, const SchemaRegistry& registry,
                              const SchemaReleaseMap& release,
                              const SerializeOptions& options = {});

/// Scans project metadata and structural counts without constructing Project.
///
/// The input is borrowed only for the call. Structural quotas, exact envelopes,
/// schema versions, UTF-8, and numeric forms are validated; cross-item model
/// references are not resolved.
///
/// @param json Complete project JSON, borrowed for the call.
/// @param registry Structural schema contracts and migrations.
/// @param limits Parser, migration, and structural quotas.
/// @return Validated metadata and counts, or a structured persistence error.
runtime::Result<ProjectSnapshotSummary, PersistenceError>
peek_project_summary(std::string_view json, const SchemaRegistry& registry,
                     const DecodeLimits& limits = {});

/// Decodes canonical Timeline JSON into a validated Project.
///
/// The input is copied as needed and need not outlive the call. Parsing,
/// migrations, structural quotas, registered content, and model invariants are
/// validated before a Project is returned.
///
/// @param json Complete project JSON, borrowed for the call.
/// @param registry Structural and extension schema contracts.
/// @param limits Parser, migration, opaque-content, and structural quotas.
/// @return A validated immutable Project or a structured persistence error.
runtime::Result<Project, PersistenceError> deserialize_project(std::string_view json,
                                                               const SchemaRegistry& registry,
                                                               const DecodeLimits& limits = {});

/// Decodes a JSON command array through the supplied schema registry.
///
/// The complete array and all nested values are subject to DecodeLimits.
///
/// @param json Complete command-envelope array JSON, borrowed for the call.
/// @param registry Command schema contracts and migrations.
/// @param limits Parser, migration, and value quotas.
/// @return Ordered commands or a structured persistence error.
runtime::Result<std::vector<Command>, PersistenceError>
deserialize_commands(std::string_view json, const SchemaRegistry& registry,
                     const DecodeLimits& limits = {});

/// @}

} // namespace pulp::timeline
