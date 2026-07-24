#pragma once

#include <pulp/timeline/schema_registry.hpp>

namespace pulp::timeline::detail {

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_take_lane_v1_to_v2(std::string_view source, BoundedJsonSink& output,
                           const void* context) noexcept;

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_take_lane_v2_to_v1(std::string_view source, BoundedJsonSink& output,
                           const void* context) noexcept;

} // namespace pulp::timeline::detail
