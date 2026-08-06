#pragma once

#include <pulp/timeline/schema_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace timeline_test_support {

/// The current version of a registered document type, read from the registry.
///
/// A schema bump used to move a spray of literal version numbers across
/// unrelated persistence suites, and a missed one fails as an unfindable
/// substring rather than as a version disagreement — which says nothing about
/// what actually changed. Derive it instead.
inline std::uint32_t current_schema_version(std::string_view type_name) {
    auto registry = pulp::timeline::make_builtin_timeline_registry();
    REQUIRE(registry.has_value());
    const auto* schema =
        registry.value().find(pulp::timeline::SchemaDomain::Document, type_name);
    REQUIRE(schema != nullptr);
    return schema->current_version;
}

/// The exact `type_name`/`version` pair an encoder writes for a document type.
///
/// Qualified by the type name on purpose: a bare `"version":N` substring watches
/// whichever envelope happens to sit at that number, which is a different
/// assertion every time an unrelated type is bumped.
inline std::string version_stamp(std::string_view type_name) {
    return "\"type_name\":\"" + std::string(type_name) +
           "\",\"version\":" + std::to_string(current_schema_version(type_name));
}

/// The stamp every track-shaped persistence suite asserts on.
inline std::string track_version_stamp() {
    return version_stamp("pulp.timeline.track");
}

} // namespace timeline_test_support
