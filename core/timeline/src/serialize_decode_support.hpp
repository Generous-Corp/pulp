#pragma once

#include <pulp/timeline/schema_registry.hpp>

namespace pulp::timeline::detail {

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
