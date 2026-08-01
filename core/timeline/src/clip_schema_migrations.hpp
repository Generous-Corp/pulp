#pragma once

#include <pulp/timeline/schema_registry.hpp>

namespace pulp::timeline::detail {

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_clip_v1_to_v2(std::string_view source, BoundedJsonSink& output,
                      const void* context) noexcept;

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_clip_v2_to_v1(std::string_view source, BoundedJsonSink& output,
                      const void* context) noexcept;

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_clip_v2_to_v3(std::string_view source, BoundedJsonSink& output,
                      const void* context) noexcept;

/// Fails when the clip carries a non-linear fade shape. A v2 reader has no
/// field to put it in and applies a linear ramp, which is an audible change to
/// the render rather than a dropped annotation, so the downgrade refuses
/// instead of silently rewriting the document's sound.
runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_clip_v3_to_v2(std::string_view source, BoundedJsonSink& output,
                      const void* context) noexcept;

} // namespace pulp::timeline::detail
