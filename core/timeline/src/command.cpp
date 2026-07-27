#include <pulp/timeline/command.hpp>

#include "sequence_scene_internal.hpp"

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
           std::bit_cast<std::uint32_t>(lhs.value) == std::bit_cast<std::uint32_t>(rhs.value) &&
           lhs.interpolation == rhs.interpolation &&
           std::bit_cast<std::uint32_t>(lhs.curvature) ==
               std::bit_cast<std::uint32_t>(rhs.curvature);
}

// Coalescing and undo squashing both hinge on this: content that compares equal
// is content the journal is allowed to drop an entry for. An alternative with no
// branch here would have to answer "equal" or "unequal" by default, and both
// answers are wrong — equal loses an edit, unequal defeats coalescing forever.
// The visit runs after the index check, so both sides hold the same alternative.
bool equal_content(const ClipContent& lhs, const ClipContent& rhs) noexcept {
    if (lhs.index() != rhs.index())
        return false;
    return std::visit(
        ClipContentCases{
            [](const EmptyContent&) { return true; },
            [&](const MediaRef& left) {
                const auto& right = std::get<MediaRef>(rhs);
                return left.asset_id == right.asset_id && left.source_start == right.source_start &&
                       left.frame_count == right.frame_count;
            },
            [&](const NoteContent& left) {
                const auto& other = std::get<NoteContent>(rhs);
                const auto right = other.notes();
                // The modifiers and the seed are part of how the clip plays, so
                // a change to either is an edit the journal must keep.
                return left.notes().size() == right.size() &&
                       std::equal(left.notes().begin(), left.notes().end(), right.begin(),
                                  equal_note) &&
                       left.modifier_seed() == other.modifier_seed() &&
                       left.modifiers().size() == other.modifiers().size() &&
                       std::equal(left.modifiers().begin(), left.modifiers().end(),
                                  other.modifiers().begin());
            },
            [&](const RegisteredContent& left) {
                const auto& right = std::get<RegisteredContent>(rhs);
                return left.schema() == right.schema() &&
                       left.canonical_payload_json() == right.canonical_payload_json();
            },
            [&](const OpaqueContent& left) {
                const auto& right = std::get<OpaqueContent>(rhs);
                return left.schema() == right.schema() && left.raw_json() == right.raw_json();
            },
            [&](const SequenceRef& left) {
                return left == std::get<SequenceRef>(rhs);
            },
        },
        lhs);
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

// This feeds the journal's retained-memory budget, so an alternative with no
// branch here reports its payload as free and lets the journal grow past the
// limit the caller asked for. Each alternative states its own cost.
std::size_t clip_retained_size(const Clip& clip) noexcept {
    return std::visit(
        ClipContentCases{
            [](const EmptyContent&) { return sizeof(Clip); },
            [](const MediaRef&) { return sizeof(Clip); },
            [](const NoteContent& notes) {
                return saturated_add(
                    saturated_add(sizeof(Clip),
                                  saturated_multiply(notes.notes().size(), sizeof(NoteEvent))),
                    saturated_multiply(notes.modifiers().size(), sizeof(NoteModifier)));
            },
            [](const RegisteredContent& registered) {
                return saturated_add(sizeof(Clip), registered.retained_bytes());
            },
            [](const OpaqueContent& opaque) {
                return saturated_add(sizeof(Clip), opaque.raw_json().size());
            },
            [](const SequenceRef&) { return sizeof(Clip); },
        },
        clip.content());
}

std::size_t automation_lane_retained_size(const AutomationLane& lane) noexcept {
    return saturated_add(sizeof(AutomationLane),
                         saturated_multiply(lane.curve().points().size(), sizeof(AutomationPoint)));
}

bool equal_take(const Take& lhs, const Take& rhs) noexcept {
    return lhs.id() == rhs.id() && lhs.media().asset_id == rhs.media().asset_id &&
           lhs.media().source_start == rhs.media().source_start &&
           lhs.media().frame_count == rhs.media().frame_count &&
           lhs.placement_start() == rhs.placement_start() && lhs.sample_rate() == rhs.sample_rate();
}

std::size_t take_lane_retained_size(const TakeLane& lane) noexcept {
    auto size = saturated_add(saturated_add(sizeof(TakeLane), lane.name().size()),
                              saturated_multiply(lane.takes().size(), sizeof(Take)));
    return saturated_add(size,
                         saturated_multiply(lane.comp_segments().size(), sizeof(TakeCompSegment)));
}

bool equal_marker(const SequenceMarker& lhs, const SequenceMarker& rhs) noexcept {
    return lhs.id == rhs.id && lhs.name == rhs.name && lhs.position == rhs.position &&
           lhs.color == rhs.color;
}

bool equal_region(const SequenceRegion& lhs, const SequenceRegion& rhs) noexcept {
    return lhs.id == rhs.id && lhs.name == rhs.name && lhs.position == rhs.position &&
           lhs.duration == rhs.duration && lhs.color == rhs.color;
}

bool equal_locators(std::span<const AssetLocator> lhs, std::span<const AssetLocator> rhs) noexcept {
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

bool equal_asset(const MediaAsset& lhs, const MediaAsset& rhs) noexcept {
    if (lhs.id != rhs.id || lhs.name != rhs.name || lhs.frame_count != rhs.frame_count ||
        lhs.sample_rate != rhs.sample_rate || lhs.content_hash != rhs.content_hash ||
        lhs.storage_policy != rhs.storage_policy || !equal_locators(lhs.locators, rhs.locators) ||
        lhs.representations.size() != rhs.representations.size() || lhs.loop_info != rhs.loop_info)
        return false;
    for (std::size_t i = 0; i < lhs.representations.size(); ++i) {
        const auto& left = lhs.representations[i];
        const auto& right = rhs.representations[i];
        if (left.role != right.role || left.content_hash != right.content_hash ||
            left.storage_policy != right.storage_policy ||
            !equal_locators(left.locators, right.locators))
            return false;
    }
    return true;
}

std::size_t asset_retained_size(const MediaAsset& asset) noexcept {
    std::size_t size = asset.name.size();
    for (const auto& locator : asset.locators)
        size = saturated_add(size, locator.hint.size());
    for (const auto& representation : asset.representations) {
        size = saturated_add(size, representation.role.size());
        for (const auto& locator : representation.locators)
            size = saturated_add(size, locator.hint.size());
    }
    if (asset.loop_info) {
        size = saturated_add(
            size, saturated_multiply(asset.loop_info->points.size(), sizeof(AudioLoopPoint)));
        size = saturated_add(size,
                             saturated_multiply(asset.loop_info->tags.size(), sizeof(std::string)));
        for (const auto& tag : asset.loop_info->tags)
            size = saturated_add(size, tag.size());
    }
    return size;
}

bool equal_absolute_duration(const std::optional<AbsoluteTimelineDuration>& lhs,
                             const std::optional<AbsoluteTimelineDuration>& rhs) noexcept {
    return lhs.has_value() == rhs.has_value() &&
           (!lhs || (lhs->sample_count == rhs->sample_count &&
                     lhs->sample_rate == rhs->sample_rate));
}

bool equal_sequence(const Sequence& lhs, const Sequence& rhs) noexcept {
    if (lhs.id() != rhs.id() || lhs.name() != rhs.name() || lhs.duration() != rhs.duration() ||
        !equal_absolute_duration(lhs.absolute_duration(), rhs.absolute_duration()) ||
        lhs.tracks().size() != rhs.tracks().size() ||
        lhs.markers().size() != rhs.markers().size() ||
        lhs.regions().size() != rhs.regions().size() ||
        lhs.chord_scale_lane() != rhs.chord_scale_lane() ||
        lhs.groove() != rhs.groove() ||
        !std::equal(lhs.markers().begin(), lhs.markers().end(), rhs.markers().begin(),
                    equal_marker) ||
        !std::equal(lhs.regions().begin(), lhs.regions().end(), rhs.regions().begin(),
                    equal_region))
        return false;
    for (std::size_t index = 0; index < lhs.tracks().size(); ++index) {
        const auto& left = lhs.tracks()[index];
        const auto& right = rhs.tracks()[index];
        if (left.id() != right.id() || left.name() != right.name() ||
            left.device_chain().size() != right.device_chain().size() ||
            left.clips().size() != right.clips().size() ||
            left.automation_lanes().size() != right.automation_lanes().size() ||
            left.take_lanes().size() != right.take_lanes().size() ||
            left.record_armed() != right.record_armed() ||
            left.active_take_lane_id() != right.active_take_lane_id() ||
            left.freeze() != right.freeze() ||
            !std::equal(left.device_chain().begin(), left.device_chain().end(),
                        right.device_chain().begin()) ||
            !std::equal(left.take_lanes().begin(), left.take_lanes().end(),
                        right.take_lanes().begin(),
                        [](const TakeLane& a, const TakeLane& b) {
                            return equivalent(a, b);
                        }))
            return false;
        for (std::size_t clip = 0; clip < left.clips().size(); ++clip)
            if (!equivalent(left.clips()[clip], right.clips()[clip]))
                return false;
        for (std::size_t lane = 0; lane < left.automation_lanes().size(); ++lane)
            if (!equivalent(left.automation_lanes()[lane], right.automation_lanes()[lane]))
                return false;
    }
    return true;
}

std::size_t sequence_retained_size(const Sequence& sequence) noexcept {
    auto size = saturated_add(sizeof(Sequence), sequence.name().size());
    size = saturated_add(
        size, saturated_multiply(sequence.markers().size(), sizeof(SequenceMarker)));
    for (const auto& marker : sequence.markers())
        size = saturated_add(size, marker.name.size());
    size = saturated_add(
        size, saturated_multiply(sequence.regions().size(), sizeof(SequenceRegion)));
    for (const auto& region : sequence.regions())
        size = saturated_add(size, region.name.size());
    size = saturated_add(
        size, saturated_multiply(sequence.chord_scale_lane().events().size(),
                                 sizeof(ChordScaleEvent)));
    size = saturated_add(size, sequence.groove().name().size());
    size = saturated_add(
        size, saturated_multiply(sequence.groove().steps().size(), sizeof(GrooveStep)));
    size = saturated_add(
        size, saturated_multiply(sequence.outgoing_sequence_refs().size(), sizeof(ItemId)));
    for (const auto& track : sequence.tracks()) {
        size = saturated_add(size, sizeof(Track));
        size = saturated_add(size, track.name().size());
        size = saturated_add(
            size, saturated_multiply(track.device_chain().size(), sizeof(DevicePlacement)));
        for (const auto& clip : track.clips())
            size = saturated_add(size, clip_retained_size(clip));
        for (const auto& lane : track.automation_lanes())
            size = saturated_add(size, automation_lane_retained_size(lane));
        for (const auto& lane : track.take_lanes())
            size = saturated_add(size, take_lane_retained_size(lane));
    }
    return size;
}

} // namespace

runtime::Result<Transaction, ModelError>
build_diverge_transaction(const Project& project, ItemLocation location,
                          TransactionId transaction_id, DocumentRevision expected_revision,
                          CommandId clone_command_id, CommandId retarget_command_id,
                          std::optional<UndoGroupId> undo_group) {
    const auto invalid = [&](ModelErrorCode code, ItemId item = {}, ItemId related = {}) {
        return runtime::Result<Transaction, ModelError>(
            runtime::Err(ModelError{code, item, related}));
    };
    if (!transaction_id.valid() || !clone_command_id.valid() ||
        !retarget_command_id.valid() ||
        transaction_id.writer != clone_command_id.writer ||
        transaction_id.writer != retarget_command_id.writer ||
        clone_command_id == retarget_command_id)
        return invalid(ModelErrorCode::InvalidItemId);
    if (!location.active || location.kind != ItemKind::Clip)
        return invalid(ModelErrorCode::MissingItem, location.clip_id);
    const auto* sequence = project.find_sequence(location.sequence_id);
    const auto* track = sequence ? sequence->find_track(location.track_id) : nullptr;
    const auto* clip = track ? track->find_clip(location.clip_id) : nullptr;
    const auto* reference =
        clip ? std::get_if<SequenceRef>(&clip->content()) : nullptr;
    if (!reference)
        return invalid(ModelErrorCode::MissingSequenceReference, location.clip_id);
    const auto* source = project.find_sequence(reference->sequence_id);
    if (!source)
        return invalid(ModelErrorCode::MissingSequenceReference, location.clip_id,
                       reference->sequence_id);
    auto allocator = project.item_id_allocator();
    auto cloned = remap_ids(*source, allocator);
    if (!cloned)
        return runtime::Result<Transaction, ModelError>(runtime::Err(cloned.error()));
    const auto cloned_id = cloned->sequence.id();
    std::vector<std::pair<ItemId, ItemId>> mapping(cloned->ids.entries().begin(),
                                                   cloned->ids.entries().end());
    Transaction result;
    result.id = transaction_id;
    result.expected_revision = expected_revision;
    result.undo_group = undo_group;
    result.gesture_phase = GesturePhase::Single;
    result.commands.push_back(
        {clone_command_id,
         CloneSequence{source->id(), cloned_id, std::move(mapping)}});
    result.commands.push_back(
        {retarget_command_id,
         SetClipSequenceRef{location.sequence_id, location.track_id, location.clip_id,
                            *reference, SequenceRef{cloned_id, reference->source_start}}});
    return runtime::Ok(std::move(result));
}

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

bool equivalent(const TakeLane& lhs, const TakeLane& rhs) noexcept {
    const auto left = lhs.takes();
    const auto right = rhs.takes();
    return lhs.id() == rhs.id() && lhs.name() == rhs.name() && left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), equal_take) &&
           std::ranges::equal(lhs.comp_segments(), rhs.comp_segments());
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
            } else if constexpr (std::is_same_v<T, ReplaceNoteContent>) {
                return left.sequence_id == right.sequence_id && left.track_id == right.track_id &&
                       left.clip_id == right.clip_id &&
                       left.expected.size() == right.expected.size() &&
                       std::equal(left.expected.begin(), left.expected.end(),
                                  right.expected.begin(), equal_note) &&
                       left.replacement.size() == right.replacement.size() &&
                       std::equal(left.replacement.begin(), left.replacement.end(),
                                  right.replacement.begin(), equal_note);
            } else if constexpr (std::is_same_v<T, SetClipPlaybackProperties>) {
                return left.sequence_id == right.sequence_id && left.track_id == right.track_id &&
                       left.clip_id == right.clip_id && left.expected == right.expected &&
                       left.replacement == right.replacement;
            } else if constexpr (std::is_same_v<T, SetTempoMap> || std::is_same_v<T, SetMeterMap>) {
                return left.expected == right.expected && left.replacement == right.replacement;
            } else if constexpr (std::is_same_v<T, CreateAsset>) {
                return equal_asset(left.asset, right.asset);
            } else if constexpr (std::is_same_v<T, RemoveAsset>) {
                return left.asset_id == right.asset_id;
            } else if constexpr (std::is_same_v<T, InsertTakeLane>) {
                return left.sequence_id == right.sequence_id && left.track_id == right.track_id &&
                       equivalent(left.lane, right.lane);
            } else if constexpr (std::is_same_v<T, RemoveTakeLane>) {
                return left.sequence_id == right.sequence_id && left.track_id == right.track_id &&
                       left.lane_id == right.lane_id;
            } else if constexpr (std::is_same_v<T, SetRecordArm>) {
                return left.sequence_id == right.sequence_id && left.track_id == right.track_id &&
                       left.expected == right.expected && left.replacement == right.replacement;
            } else if constexpr (std::is_same_v<T, InsertTake>) {
                return left.sequence_id == right.sequence_id && left.track_id == right.track_id &&
                       left.lane_id == right.lane_id && equal_take(left.take, right.take);
            } else if constexpr (std::is_same_v<T, RemoveTake>) {
                return left.sequence_id == right.sequence_id && left.track_id == right.track_id &&
                       left.lane_id == right.lane_id && left.take_id == right.take_id;
            } else if constexpr (std::is_same_v<T, SetActiveTakeLane>) {
                return left.sequence_id == right.sequence_id && left.track_id == right.track_id &&
                       left.expected_lane_id == right.expected_lane_id &&
                       left.replacement_lane_id == right.replacement_lane_id;
            } else if constexpr (std::is_same_v<T, SetTakeComp>) {
                return left.sequence_id == right.sequence_id && left.track_id == right.track_id &&
                       left.lane_id == right.lane_id && left.expected == right.expected &&
                       left.replacement == right.replacement;
            } else if constexpr (std::is_same_v<T, SetTrackFreeze>) {
                return left.sequence_id == right.sequence_id && left.track_id == right.track_id &&
                       left.expected == right.expected && left.replacement == right.replacement;
            } else if constexpr (std::is_same_v<T, SetChordScaleLane> ||
                                 std::is_same_v<T, SetGroove>) {
                return left.sequence_id == right.sequence_id && left.expected == right.expected &&
                       left.replacement == right.replacement;
            } else if constexpr (std::is_same_v<T, InsertMarker>) {
                return left.sequence_id == right.sequence_id &&
                       equal_marker(left.marker, right.marker);
            } else if constexpr (std::is_same_v<T, RemoveMarker>) {
                return left.sequence_id == right.sequence_id && left.marker_id == right.marker_id;
            } else if constexpr (std::is_same_v<T, InsertRegion>) {
                return left.sequence_id == right.sequence_id &&
                       equal_region(left.region, right.region);
            } else if constexpr (std::is_same_v<T, RemoveRegion>) {
                return left.sequence_id == right.sequence_id && left.region_id == right.region_id;
            } else if constexpr (std::is_same_v<T, InsertScene>) {
                return left.sequence_id == right.sequence_id && left.scene == right.scene &&
                       left.before_scene_id == right.before_scene_id;
            } else if constexpr (std::is_same_v<T, RemoveScene>) {
                return left.sequence_id == right.sequence_id && left.scene_id == right.scene_id;
            } else if constexpr (std::is_same_v<T, InsertSlot>) {
                return left.sequence_id == right.sequence_id && left.scene_id == right.scene_id &&
                       left.slot == right.slot && left.before_slot_id == right.before_slot_id;
            } else if constexpr (std::is_same_v<T, RemoveSlot>) {
                return left.sequence_id == right.sequence_id && left.scene_id == right.scene_id &&
                       left.slot_id == right.slot_id;
            } else if constexpr (std::is_same_v<T, InsertSequence>) {
                return equal_sequence(left.sequence, right.sequence);
            } else if constexpr (std::is_same_v<T, CloneSequence>) {
                return left.source_sequence_id == right.source_sequence_id &&
                       left.cloned_sequence_id == right.cloned_sequence_id &&
                       left.id_remap == right.id_remap;
            } else if constexpr (std::is_same_v<T, RemoveSequence>) {
                return left.sequence_id == right.sequence_id;
            } else if constexpr (std::is_same_v<T, SetClipSequenceRef>) {
                return left.sequence_id == right.sequence_id &&
                       left.track_id == right.track_id && left.clip_id == right.clip_id &&
                       left.expected == right.expected &&
                       left.replacement == right.replacement;
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
            if constexpr (std::is_same_v<T, CreateAsset>)
                return saturated_add(sizeof(T), asset_retained_size(value.asset));
            if constexpr (std::is_same_v<T, InsertAutomationLane>)
                return saturated_add(sizeof(T), automation_lane_retained_size(value.lane));
            if constexpr (std::is_same_v<T, InsertTakeLane>)
                return saturated_add(sizeof(T), take_lane_retained_size(value.lane));
            if constexpr (std::is_same_v<T, InsertTake>)
                return sizeof(T);
            if constexpr (std::is_same_v<T, InsertMarker>)
                return saturated_add(sizeof(T), value.marker.name.size());
            if constexpr (std::is_same_v<T, InsertRegion>)
                return saturated_add(sizeof(T), value.region.name.size());
            if constexpr (std::is_same_v<T, InsertScene>)
                return saturated_add(
                    saturated_add(sizeof(T), value.scene.name.size()),
                    detail::launcher_slot_list_owned_storage(value.scene.slots));
            if constexpr (std::is_same_v<T, InsertSequence>)
                return saturated_add(sizeof(T), sequence_retained_size(value.sequence));
            if constexpr (std::is_same_v<T, CloneSequence>)
                return saturated_add(
                    sizeof(T),
                    saturated_multiply(value.id_remap.size(),
                                       sizeof(std::pair<ItemId, ItemId>)));
            if constexpr (std::is_same_v<T, SetTakeComp>) {
                const auto segment_count =
                    saturated_add(value.expected.size(), value.replacement.size());
                return saturated_add(sizeof(T),
                                     saturated_multiply(segment_count, sizeof(TakeCompSegment)));
            }
            if constexpr (std::is_same_v<T, ReplaceNoteContent>) {
                const auto note_count =
                    saturated_add(value.expected.size(), value.replacement.size());
                return saturated_add(sizeof(T), saturated_multiply(note_count, sizeof(NoteEvent)));
            }
            if constexpr (std::is_same_v<T, SetTempoMap>)
                return saturated_add(
                    sizeof(T), saturated_multiply(saturated_add(value.expected.points().size(),
                                                                value.replacement.points().size()),
                                                  sizeof(timebase::TempoPoint)));
            if constexpr (std::is_same_v<T, SetChordScaleLane>)
                return saturated_add(
                    sizeof(T), saturated_multiply(saturated_add(value.expected.events().size(),
                                                                value.replacement.events().size()),
                                                  sizeof(ChordScaleEvent)));
            if constexpr (std::is_same_v<T, SetGroove>)
                return saturated_add(
                    sizeof(T),
                    saturated_add(saturated_multiply(saturated_add(value.expected.steps().size(),
                                                                   value.replacement.steps().size()),
                                                     sizeof(GrooveStep)),
                                  saturated_add(value.expected.name().size(),
                                                value.replacement.name().size())));
            if constexpr (std::is_same_v<T, SetMeterMap>)
                return saturated_add(
                    sizeof(T), saturated_multiply(saturated_add(value.expected.points().size(),
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
