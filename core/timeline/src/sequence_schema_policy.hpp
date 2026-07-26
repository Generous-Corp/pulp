#pragma once

#include <cstdint>
#include <string_view>

namespace pulp::timeline::detail {

struct SequenceSchemaVersionPolicy {
    std::string_view type_name;
    std::uint32_t oldest_readable_version;
    std::uint32_t current_version;
    std::uint32_t annotations_introduced_version;

    // Markers and regions entered the sequence schema together, so one predicate
    // governs both arrays: a version that carries either must carry both.
    [[nodiscard]] constexpr bool requires_annotations(std::uint32_t version) const noexcept {
        return version >= annotations_introduced_version;
    }
};

inline constexpr SequenceSchemaVersionPolicy sequence_schema_policy{
    "pulp.timeline.sequence",
    1,
    2,
    2,
};
static_assert(sequence_schema_policy.oldest_readable_version > 0 &&
              sequence_schema_policy.oldest_readable_version <=
                  sequence_schema_policy.current_version &&
              sequence_schema_policy.annotations_introduced_version > 0 &&
              sequence_schema_policy.annotations_introduced_version <=
                  sequence_schema_policy.current_version &&
              !sequence_schema_policy.requires_annotations(
                  sequence_schema_policy.annotations_introduced_version - 1) &&
              sequence_schema_policy.requires_annotations(
                  sequence_schema_policy.annotations_introduced_version));

} // namespace pulp::timeline::detail
