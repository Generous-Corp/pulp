#pragma once

#include <pulp/timeline/schema_registry.hpp>

namespace pulp::timeline::detail {

struct DecodeCounts {
    std::size_t assets = 0;
    std::size_t sequences = 0;
    std::size_t tracks = 0;
    std::size_t clips = 0;
    std::size_t notes = 0;
    std::size_t device_placements = 0;
    std::size_t automation_lanes = 0;
    std::size_t automation_points = 0;
    std::size_t sequence_markers = 0;
    std::size_t sequence_regions = 0;
};

struct StructuralData {
    const JsonValue* data = nullptr;
    std::uint32_t version = 0;
};

runtime::Result<const JsonValue*, PersistenceError>
required(const JsonValue& object, std::string_view name, std::string path);
runtime::Result<std::string, PersistenceError>
string_field(const JsonValue& object, std::string_view name, std::string path);
runtime::Result<StructuralData, PersistenceError>
data_for_versions(const JsonValue& value, std::string_view expected_type,
                  std::uint32_t minimum_version, std::uint32_t maximum_version, std::string path);
runtime::Result<const JsonValue*, PersistenceError>
data_for(const JsonValue& value, std::string_view expected_type, std::string path);
runtime::Result<timebase::RationalRate, PersistenceError> decode_rate(const JsonValue& value,
                                                                      std::string path);

} // namespace pulp::timeline::detail
