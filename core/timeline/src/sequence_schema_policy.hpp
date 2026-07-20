#pragma once

#include "track_schema_policy.hpp"

namespace pulp::timeline::detail {

inline constexpr std::uint32_t sequence_annotations_version = 2;
constexpr bool sequence_has_annotations(std::uint32_t version) noexcept {
    return version >= sequence_annotations_version;
}
inline constexpr StructuralSchemaVersionPolicy sequence_schema_policy{
    "pulp.timeline.sequence", 1, sequence_annotations_version};

} // namespace pulp::timeline::detail
