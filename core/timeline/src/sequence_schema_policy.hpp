#pragma once

#include <cstdint>
#include <string_view>

namespace pulp::timeline::detail {

struct SequenceSchemaVersionPolicy {
    std::string_view type_name;
    std::uint32_t oldest_readable_version;
    std::uint32_t current_version;
    std::uint32_t annotations_introduced_version;
    std::uint32_t chord_scale_lane_introduced_version;
    std::uint32_t groove_introduced_version;
    std::uint32_t scenes_introduced_version;
    std::uint32_t track_order_introduced_version;
    std::uint32_t chord_detail_introduced_version;
    std::uint32_t section_role_introduced_version;

    // Markers and regions entered the sequence schema together, so one predicate
    // governs both arrays: a version that carries either must carry both.
    [[nodiscard]] constexpr bool requires_annotations(std::uint32_t version) const noexcept {
        return version >= annotations_introduced_version;
    }

    [[nodiscard]] constexpr bool requires_chord_scale_lane(std::uint32_t version) const noexcept {
        return version >= chord_scale_lane_introduced_version;
    }

    [[nodiscard]] constexpr bool requires_groove(std::uint32_t version) const noexcept {
        return version >= groove_introduced_version;
    }

    [[nodiscard]] constexpr bool requires_scenes(std::uint32_t version) const noexcept {
        return version >= scenes_introduced_version;
    }

    [[nodiscard]] constexpr bool requires_track_order(std::uint32_t version) const noexcept {
        return version >= track_order_introduced_version;
    }

    // The bass, extension mask, and voicing hint a chord event carries beyond
    // its quality and root. They entered together and are always written
    // together, so one predicate governs all three.
    [[nodiscard]] constexpr bool requires_chord_detail(std::uint32_t version) const noexcept {
        return version >= chord_detail_introduced_version;
    }

    [[nodiscard]] constexpr bool requires_section_role(std::uint32_t version) const noexcept {
        return version >= section_role_introduced_version;
    }
};

// Chord detail and section roles entered the schema in the same version. They
// are separate predicates rather than one because they govern different arrays
// and a later version may move only one of them.
inline constexpr SequenceSchemaVersionPolicy sequence_schema_policy{
    "pulp.timeline.sequence", 1, 7, 2, 3, 4, 5, 6, 7, 7,
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
                  sequence_schema_policy.annotations_introduced_version) &&
              sequence_schema_policy.chord_scale_lane_introduced_version > 0 &&
              sequence_schema_policy.chord_scale_lane_introduced_version <=
                  sequence_schema_policy.current_version &&
              !sequence_schema_policy.requires_chord_scale_lane(
                  sequence_schema_policy.chord_scale_lane_introduced_version - 1) &&
              sequence_schema_policy.requires_chord_scale_lane(
                  sequence_schema_policy.chord_scale_lane_introduced_version));
static_assert(sequence_schema_policy.groove_introduced_version >
                  sequence_schema_policy.chord_scale_lane_introduced_version &&
              sequence_schema_policy.groove_introduced_version <=
                  sequence_schema_policy.current_version &&
              !sequence_schema_policy.requires_groove(
                  sequence_schema_policy.groove_introduced_version - 1) &&
              sequence_schema_policy.requires_groove(
                  sequence_schema_policy.groove_introduced_version) &&
              sequence_schema_policy.scenes_introduced_version >
                  sequence_schema_policy.groove_introduced_version &&
              sequence_schema_policy.scenes_introduced_version <=
                  sequence_schema_policy.current_version &&
              !sequence_schema_policy.requires_scenes(
                  sequence_schema_policy.scenes_introduced_version - 1) &&
              sequence_schema_policy.requires_scenes(
                  sequence_schema_policy.scenes_introduced_version));
static_assert(sequence_schema_policy.track_order_introduced_version >
                  sequence_schema_policy.scenes_introduced_version &&
              sequence_schema_policy.track_order_introduced_version <=
                  sequence_schema_policy.current_version &&
              !sequence_schema_policy.requires_track_order(
                  sequence_schema_policy.track_order_introduced_version - 1) &&
              sequence_schema_policy.requires_track_order(
                  sequence_schema_policy.track_order_introduced_version));
static_assert(sequence_schema_policy.chord_detail_introduced_version >
                  sequence_schema_policy.track_order_introduced_version &&
              sequence_schema_policy.chord_detail_introduced_version <=
                  sequence_schema_policy.current_version &&
              !sequence_schema_policy.requires_chord_detail(
                  sequence_schema_policy.chord_detail_introduced_version - 1) &&
              sequence_schema_policy.requires_chord_detail(
                  sequence_schema_policy.chord_detail_introduced_version) &&
              sequence_schema_policy.section_role_introduced_version >
                  sequence_schema_policy.track_order_introduced_version &&
              sequence_schema_policy.section_role_introduced_version <=
                  sequence_schema_policy.current_version &&
              !sequence_schema_policy.requires_section_role(
                  sequence_schema_policy.section_role_introduced_version - 1) &&
              sequence_schema_policy.requires_section_role(
                  sequence_schema_policy.section_role_introduced_version));

// The groove a sequence carries when it states no feel, in canonical field
// order. The upgrade that introduces the field writes exactly this, and the
// matching downgrade erases exactly this and refuses anything else, so both
// directions are stated once rather than twice.
inline constexpr std::string_view kStraightGrooveJson =
    "{\"name\":\"\",\"step\":\"0\",\"steps\":[],\"swing_denominator\":\"2\",\"swing_grid\":\"0\","
    "\"swing_numerator\":\"1\",\"timing_strength\":1000,\"velocity_strength\":1000}";

} // namespace pulp::timeline::detail
