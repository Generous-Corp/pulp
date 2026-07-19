#include "snapshot_equivalence.hpp"

#include "project_state_access.hpp"

#include <pulp/timeline/command.hpp>

#include <algorithm>
#include <optional>

namespace pulp::timeline::detail {
namespace {

bool same_absolute_duration(const std::optional<AbsoluteTimelineDuration>& lhs,
                            const std::optional<AbsoluteTimelineDuration>& rhs) noexcept {
    return lhs.has_value() == rhs.has_value() && (!lhs || (lhs->sample_count == rhs->sample_count &&
                                                           lhs->sample_rate == rhs->sample_rate));
}

bool same_representation(const AssetRepresentation& lhs, const AssetRepresentation& rhs) noexcept {
    return lhs.role == rhs.role && lhs.content_hash == rhs.content_hash &&
           lhs.storage_policy == rhs.storage_policy && lhs.locators == rhs.locators;
}

bool same_asset(const MediaAsset& lhs, const MediaAsset& rhs) noexcept {
    return lhs.id == rhs.id && lhs.name == rhs.name && lhs.frame_count == rhs.frame_count &&
           lhs.sample_rate == rhs.sample_rate && lhs.content_hash == rhs.content_hash &&
           lhs.storage_policy == rhs.storage_policy && lhs.locators == rhs.locators &&
           lhs.representations.size() == rhs.representations.size() &&
           std::equal(lhs.representations.begin(), lhs.representations.end(),
                      rhs.representations.begin(), same_representation);
}

} // namespace

bool snapshots_equivalent(const Project& lhs, const Project& rhs) noexcept {
    if (lhs.id() != rhs.id() || lhs.name() != rhs.name() ||
        lhs.next_item_id() != rhs.next_item_id() ||
        lhs.root_sequence_id() != rhs.root_sequence_id() || lhs.tempo_map() != rhs.tempo_map() ||
        lhs.meter_map() != rhs.meter_map() || lhs.assets().size() != rhs.assets().size() ||
        lhs.sequences().size() != rhs.sequences().size())
        return false;
    for (std::size_t i = 0; i < lhs.assets().size(); ++i) {
        const auto& left = lhs.assets()[i];
        const auto& right = rhs.assets()[i];
        if (!same_asset(left, right))
            return false;
    }
    for (std::size_t s = 0; s < lhs.sequences().size(); ++s) {
        const auto& left_sequence = lhs.sequences()[s];
        const auto& right_sequence = rhs.sequences()[s];
        if (left_sequence.id() != right_sequence.id() ||
            left_sequence.name() != right_sequence.name() ||
            left_sequence.duration() != right_sequence.duration() ||
            !same_absolute_duration(left_sequence.absolute_duration(),
                                    right_sequence.absolute_duration()) ||
            left_sequence.tracks().size() != right_sequence.tracks().size())
            return false;
        for (std::size_t t = 0; t < left_sequence.tracks().size(); ++t) {
            const auto& left_track = left_sequence.tracks()[t];
            const auto& right_track = right_sequence.tracks()[t];
            if (left_track.id() != right_track.id() || left_track.name() != right_track.name() ||
                left_track.clips().size() != right_track.clips().size() ||
                left_track.device_chain().size() != right_track.device_chain().size() ||
                !std::equal(left_track.device_chain().begin(), left_track.device_chain().end(),
                            right_track.device_chain().begin()))
                return false;
            for (std::size_t c = 0; c < left_track.clips().size(); ++c)
                if (!equivalent(left_track.clips()[c], right_track.clips()[c]))
                    return false;
        }
    }
    return ProjectStateAccess::identities_equivalent(lhs, rhs);
}

} // namespace pulp::timeline::detail
