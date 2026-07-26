#pragma once

#include <pulp/timeline/schema_registry.hpp>

namespace pulp::timeline::detail {

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v1_to_v2(std::string_view source, BoundedJsonSink& output,
                          const void* context) noexcept;

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v2_to_v1(std::string_view source, BoundedJsonSink& output,
                          const void* context) noexcept;

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v2_to_v3(std::string_view source, BoundedJsonSink& output,
                          const void* context) noexcept;

// Refuses when the lane carries events. A downgrade that dropped them would
// change what the document sounds like while reporting success.
runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v3_to_v2(std::string_view source, BoundedJsonSink& output,
                          const void* context) noexcept;

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v3_to_v4(std::string_view source, BoundedJsonSink& output,
                          const void* context) noexcept;

// Refuses when the groove states anything other than the straight feel or the
// scene list is non-empty. Dropping either would change authored playback while
// reporting success.
runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v4_to_v3(std::string_view source, BoundedJsonSink& output,
                          const void* context) noexcept;

} // namespace pulp::timeline::detail
