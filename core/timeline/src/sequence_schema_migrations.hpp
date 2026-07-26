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

// Refuses when the groove states anything other than the straight feel. A
// downgrade that dropped a swing setting or a step table would change when
// every note in the sequence sounds while reporting success.
runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v4_to_v3(std::string_view source, BoundedJsonSink& output,
                          const void* context) noexcept;

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v4_to_v5(std::string_view source, BoundedJsonSink& output,
                          const void* context) noexcept;

// Refuses a non-empty scene list because v4 has no launcher representation.
runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v5_to_v4(std::string_view source, BoundedJsonSink& output,
                          const void* context) noexcept;

} // namespace pulp::timeline::detail
