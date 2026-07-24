#pragma once

#include <pulp/timeline/schema_registry.hpp>

namespace pulp::timeline::detail {

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v1_to_v2(std::string_view source, BoundedJsonSink& output,
                       const void* context) noexcept;

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v2_to_v1(std::string_view source, BoundedJsonSink& output,
                       const void* context) noexcept;

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v2_to_v3(std::string_view source, BoundedJsonSink& output,
                       const void* context) noexcept;

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v3_to_v2(std::string_view source, BoundedJsonSink& output,
                       const void* context) noexcept;

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v3_to_v4(std::string_view source, BoundedJsonSink& output,
                       const void* context) noexcept;

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v4_to_v3(std::string_view source, BoundedJsonSink& output,
                       const void* context) noexcept;

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v4_to_v5(std::string_view source, BoundedJsonSink& output,
                       const void* context) noexcept;

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v5_to_v4(std::string_view source, BoundedJsonSink& output,
                       const void* context) noexcept;

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v5_to_v6(std::string_view source, BoundedJsonSink& output,
                       const void* context) noexcept;

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v6_to_v5(std::string_view source, BoundedJsonSink& output,
                       const void* context) noexcept;

} // namespace pulp::timeline::detail
