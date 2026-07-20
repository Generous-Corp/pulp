#include <pulp/timeline/command.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>

namespace pulp::timeline {
namespace {

bool equal_note(const NoteEvent& lhs, const NoteEvent& rhs) noexcept {
    return lhs.id == rhs.id && lhs.start == rhs.start && lhs.duration == rhs.duration &&
           lhs.velocity == rhs.velocity && lhs.pitch == rhs.pitch && lhs.channel == rhs.channel;
}

bool equal_automation_point(const AutomationPoint& lhs, const AutomationPoint& rhs) noexcept {
    return lhs.id == rhs.id && lhs.position == rhs.position &&
           std::bit_cast<std::uint32_t>(lhs.value) ==
               std::bit_cast<std::uint32_t>(rhs.value) &&
           lhs.interpolation == rhs.interpolation &&
           std::bit_cast<std::uint32_t>(lhs.curvature) ==
               std::bit_cast<std::uint32_t>(rhs.curvature);
}

bool equal_content(const ClipContent& lhs, const ClipContent& rhs) noexcept {
    if (lhs.index() != rhs.index())
        return false;
    if (std::holds_alternative<EmptyContent>(lhs))
        return true;
    if (const auto* left = std::get_if<MediaRef>(&lhs)) {
        const auto& right = std::get<MediaRef>(rhs);
        return left->asset_id == right.asset_id && left->source_start == right.source_start &&
               left->frame_count == right.frame_count;
    }
    if (const auto* left = std::get_if<NoteContent>(&lhs)) {
        const auto right = std::get<NoteContent>(rhs).notes();
        return left->notes().size() == right.size() &&
               std::equal(left->notes().begin(), left->notes().end(), right.begin(), equal_note);
    }
    if (const auto* left = std::get_if<RegisteredContent>(&lhs)) {
        const auto& right = std::get<RegisteredContent>(rhs);
        return left->schema() == right.schema() &&
               left->canonical_payload_json() == right.canonical_payload_json();
    }
    const auto& left = std::get<OpaqueContent>(lhs);
    const auto& right = std::get<OpaqueContent>(rhs);
    return left.schema() == right.schema() && left.raw_json() == right.raw_json();
}

std::size_t saturated_add(std::size_t lhs, std::size_t rhs) noexcept {
    return rhs > std::numeric_limits<std::size_t>::max() - lhs
               ? std::numeric_limits<std::size_t>::max()
               : lhs + rhs;
}

std::size_t saturated_multiply(std::size_t lhs, std::size_t rhs) noexcept {
    return lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs
               ? std::numeric_limits<std::size_t>::max()
               : lhs * rhs;
}

std::size_t clip_retained_size(const Clip& clip) noexcept {
    auto size = sizeof(Clip);
    if (const auto* notes = std::get_if<NoteContent>(&clip.content()))
        size = saturated_add(size, notes->notes().size() * sizeof(NoteEvent));
    else if (const auto* opaque = std::get_if<OpaqueContent>(&clip.content()))
        size = saturated_add(size, opaque->raw_json().size());
    else if (const auto* registered = std::get_if<RegisteredContent>(&clip.content()))
        size = saturated_add(size, registered->retained_bytes());
    return size;
}

std::size_t automation_lane_retained_size(const AutomationLane& lane) noexcept {
    return saturated_add(
        sizeof(AutomationLane),
        saturated_multiply(lane.curve().points().size(), sizeof(AutomationPoint)));
}

} // namespace

bool equivalent(const ClipTimeRange& lhs, const ClipTimeRange& rhs) noexcept {
    if (lhs.index() != rhs.index())
        return false;
    if (const auto* left = std::get_if<MusicalTimeRange>(&lhs)) {
        const auto& right = std::get<MusicalTimeRange>(rhs);
        return left->start == right.start && left->duration == right.duration;
    }
    const auto& left = std::get<AbsoluteTimeRange>(lhs);
    const auto& right = std::get<AbsoluteTimeRange>(rhs);
    return left.start == right.start && left.sample_count == right.sample_count &&
           left.sample_rate == right.sample_rate;
}

bool equivalent(const Clip& lhs, const Clip& rhs) noexcept {
    return lhs.id() == rhs.id() && equivalent(lhs.time_range(), rhs.time_range()) &&
           equal_content(lhs.content(), rhs.content()) &&
           lhs.playback_properties() == rhs.playback_properties();
}

bool equivalent(const AutomationLane& lhs, const AutomationLane& rhs) noexcept {
    const auto left = lhs.curve().points();
    const auto right = rhs.curve().points();
    return lhs.id() == rhs.id() && lhs.target() == rhs.target() && left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), equal_automation_point);
}

bool equivalent(const Command& lhs, const Command& rhs) noexcept {
    if (lhs.index() != rhs.index())
        return false;
    return std::visit(
        [&](const auto& left) {
            using T = std::decay_t<decltype(left)>;
            const auto& right = std::get<T>(rhs);
            if constexpr (std::is_same_v<T, InsertClip>) {
                return left.sequence_id == right.sequence_id && left.track_id == right.track_id &&
                       equivalent(left.clip, right.clip);
            } else if constexpr (std::is_same_v<T, RemoveClip>) {
                return left.sequence_id == right.sequence_id && left.track_id == right.track_id &&
                       left.clip_id == right.clip_id;
            } else if constexpr (std::is_same_v<T, InsertAutomationLane>) {
                return left.sequence_id == right.sequence_id && left.track_id == right.track_id &&
                       equivalent(left.lane, right.lane);
            } else if constexpr (std::is_same_v<T, RemoveAutomationLane>) {
                return left.sequence_id == right.sequence_id && left.track_id == right.track_id &&
                       left.lane_id == right.lane_id;
            } else if constexpr (std::is_same_v<T, MoveClip>) {
                return left.sequence_id == right.sequence_id && left.track_id == right.track_id &&
                       left.clip_id == right.clip_id &&
                       equivalent(left.expected_range, right.expected_range) &&
                       equivalent(left.replacement_range, right.replacement_range);
            } else if constexpr (std::is_same_v<T, SetNoteVelocity>) {
                return left.sequence_id == right.sequence_id && left.track_id == right.track_id &&
                       left.clip_id == right.clip_id && left.note_id == right.note_id &&
                       left.expected_velocity == right.expected_velocity &&
                       left.replacement_velocity == right.replacement_velocity;
            } else if constexpr (std::is_same_v<T, SetClipPlaybackProperties>) {
                return left.sequence_id == right.sequence_id && left.track_id == right.track_id &&
                       left.clip_id == right.clip_id && left.expected == right.expected &&
                       left.replacement == right.replacement;
            } else if constexpr (std::is_same_v<T, SetTempoMap> ||
                                 std::is_same_v<T, SetMeterMap>) {
                return left.expected == right.expected && left.replacement == right.replacement;
            } else {
                return left.sequence_id == right.sequence_id && left.track_id == right.track_id &&
                       left.clip_id == right.clip_id && left.expected == right.expected &&
                       left.replacement == right.replacement;
            }
        },
        lhs);
}

bool equivalent(const Transaction& lhs, const Transaction& rhs) noexcept {
    if (lhs.id != rhs.id || lhs.expected_revision != rhs.expected_revision ||
        lhs.undo_group != rhs.undo_group || lhs.gesture_phase != rhs.gesture_phase ||
        lhs.commands.size() != rhs.commands.size())
        return false;
    for (std::size_t i = 0; i < lhs.commands.size(); ++i) {
        if (lhs.commands[i].id != rhs.commands[i].id ||
            !equivalent(lhs.commands[i].command, rhs.commands[i].command))
            return false;
    }
    return true;
}

std::size_t retained_size(const Command& command) noexcept {
    return std::visit(
        [](const auto& value) -> std::size_t {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, InsertClip>)
                return saturated_add(sizeof(T), clip_retained_size(value.clip));
            if constexpr (std::is_same_v<T, InsertAutomationLane>)
                return saturated_add(sizeof(T), automation_lane_retained_size(value.lane));
            if constexpr (std::is_same_v<T, SetTempoMap>)
                return saturated_add(sizeof(T),
                                     saturated_multiply(
                                         saturated_add(value.expected.points().size(),
                                                       value.replacement.points().size()),
                                         sizeof(timebase::TempoPoint)));
            if constexpr (std::is_same_v<T, SetMeterMap>)
                return saturated_add(sizeof(T),
                                     saturated_multiply(
                                         saturated_add(value.expected.points().size(),
                                                       value.replacement.points().size()),
                                         sizeof(timebase::MeterPoint)));
            return sizeof(T);
        },
        command);
}

std::size_t retained_size(const Transaction& transaction) noexcept {
    std::size_t size = sizeof(Transaction);
    for (const auto& envelope : transaction.commands)
        size = saturated_add(
            size, saturated_add(sizeof(CommandEnvelope), retained_size(envelope.command)));
    return size;
}

} // namespace pulp::timeline
