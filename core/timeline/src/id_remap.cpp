#include <pulp/timeline/model.hpp>

#include "automation_document_internal.hpp"
#include "project_state_access.hpp"

#include <algorithm>

namespace pulp::timeline {

struct IdRemapBuilder {
    static std::vector<std::pair<ItemId, ItemId>>& entries(IdRemapTable& table) noexcept {
        return table.entries_;
    }
};

namespace {

template <typename T>
runtime::Result<T, ModelError> fail(ModelErrorCode code, ItemId item = {}, ItemId related = {}) {
    return runtime::Result<T, ModelError>(runtime::Err(ModelError{code, item, related}));
}

} // namespace

std::optional<ItemId> IdRemapTable::find(ItemId old_id) const noexcept {
    const auto found =
        std::lower_bound(entries_.begin(), entries_.end(), old_id,
                         [](const auto& entry, ItemId wanted) { return entry.first < wanted; });
    return found != entries_.end() && found->first == old_id ? std::optional<ItemId>(found->second)
                                                             : std::nullopt;
}

runtime::Result<ItemId, ModelError> ExternalIdFixup::apply(ItemId id) const noexcept {
    return map ? map(context, id) : runtime::Result<ItemId, ModelError>(runtime::Ok(id));
}

namespace {

std::optional<ModelError> allocate_owned(IdRemapTable& table, ItemIdAllocator& allocator,
                                         ItemId old_id) {
    auto next = allocator.allocate();
    if (!next)
        return next.error();
    IdRemapBuilder::entries(table).emplace_back(old_id, next.value());
    return std::nullopt;
}

std::optional<ModelError> finish_table(IdRemapTable& table) {
    auto& entries = IdRemapBuilder::entries(table);
    std::sort(entries.begin(), entries.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    const auto duplicate =
        std::adjacent_find(entries.begin(), entries.end(),
                           [](const auto& lhs, const auto& rhs) { return lhs.first == rhs.first; });
    if (duplicate != entries.end())
        return ModelError{ModelErrorCode::DuplicateItemId, duplicate->first, {}};
    return std::nullopt;
}

std::optional<ModelError> validate_owned_ids(std::vector<ItemId> ids) {
    for (const auto id : ids)
        if (!id.valid())
            return ModelError{ModelErrorCode::InvalidItemId, id, {}};
    std::sort(ids.begin(), ids.end());
    const auto duplicate = std::adjacent_find(ids.begin(), ids.end());
    if (duplicate != ids.end())
        return ModelError{ModelErrorCode::DuplicateItemId, *duplicate, {}};
    return std::nullopt;
}

void append_clip_ids(const Clip& clip, std::vector<ItemId>& ids) {
    ids.push_back(clip.id());
    if (const auto* notes = std::get_if<NoteContent>(&clip.content()))
        for (const auto& note : notes->notes())
            ids.push_back(note.id);
}

void append_take_ids(const Track& track, std::vector<ItemId>& ids) {
    for (const auto& lane : track.take_lanes()) {
        ids.push_back(lane.id());
        for (const auto& take : lane.takes())
            ids.push_back(take.id());
    }
}

std::optional<ModelError> preflight(const Clip& clip) {
    if (std::holds_alternative<OpaqueContent>(clip.content()))
        return ModelError{ModelErrorCode::OpaqueContentCannotRemap, clip.id(), {}};
    std::vector<ItemId> ids;
    append_clip_ids(clip, ids);
    return validate_owned_ids(std::move(ids));
}

std::optional<ModelError> preflight(const Track& track) {
    std::vector<ItemId> ids{track.id()};
    for (const auto& device : track.device_chain())
        ids.push_back(device.id);
    detail::append_automation_owned_ids(track.automation_lanes(), ids);
    append_take_ids(track, ids);
    for (const auto& clip : track.clips()) {
        if (std::holds_alternative<OpaqueContent>(clip.content()))
            return ModelError{ModelErrorCode::OpaqueContentCannotRemap, clip.id(), {}};
        append_clip_ids(clip, ids);
    }
    return validate_owned_ids(std::move(ids));
}

std::optional<ModelError> preflight(const Sequence& sequence) {
    std::vector<ItemId> ids{sequence.id()};
    for (const auto& track : sequence.tracks()) {
        ids.push_back(track.id());
        for (const auto& device : track.device_chain())
            ids.push_back(device.id);
        detail::append_automation_owned_ids(track.automation_lanes(), ids);
        append_take_ids(track, ids);
        for (const auto& clip : track.clips()) {
            if (std::holds_alternative<OpaqueContent>(clip.content()))
                return ModelError{ModelErrorCode::OpaqueContentCannotRemap, clip.id(), {}};
            append_clip_ids(clip, ids);
        }
    }
    return validate_owned_ids(std::move(ids));
}

runtime::Result<Clip, ModelError> rebuild_clip(const Clip& clip, const IdRemapTable& table,
                                               ExternalIdFixup external) {
    ClipContent content = clip.content();
    if (auto* media = std::get_if<MediaRef>(&content)) {
        auto fixed = external.apply(media->asset_id);
        if (!fixed)
            return fail<Clip>(fixed.error().code, fixed.error().item, fixed.error().related_item);
        media->asset_id = fixed.value();
    }
    if (const auto* old_notes = std::get_if<NoteContent>(&clip.content())) {
        std::vector<NoteEvent> notes(old_notes->notes().begin(), old_notes->notes().end());
        for (auto& note : notes)
            note.id = *table.find(note.id);
        auto rebuilt = NoteContent::create(std::move(notes));
        if (!rebuilt)
            return fail<Clip>(rebuilt.error().code, rebuilt.error().item,
                              rebuilt.error().related_item);
        content = std::move(rebuilt).value();
    }
    if (clip.time_anchor() == ClipTimeAnchor::Musical)
        return Clip::create(*table.find(clip.id()), clip.start(), clip.duration(),
                            std::move(content), clip.playback_properties());
    return Clip::create_absolute(*table.find(clip.id()), clip.absolute_start(),
                                 clip.absolute_duration_samples(), clip.absolute_sample_rate(),
                                 std::move(content), clip.playback_properties());
}

void allocate_clip_owned(const Clip& clip, IdRemapTable& table, ItemIdAllocator& allocator,
                         std::optional<ModelError>& error) {
    if (error)
        return;
    error = allocate_owned(table, allocator, clip.id());
    if (const auto* notes = std::get_if<NoteContent>(&clip.content()))
        for (const auto& note : notes->notes()) {
            if (error)
                return;
            error = allocate_owned(table, allocator, note.id);
        }
}

void allocate_automation_owned(const AutomationLane& lane, IdRemapTable& table,
                               ItemIdAllocator& allocator, std::optional<ModelError>& error) {
    if (error)
        return;
    error = allocate_owned(table, allocator, lane.id());
    for (const auto& point : lane.curve().points()) {
        if (error)
            return;
        error = allocate_owned(table, allocator, point.id);
    }
}

void allocate_take_owned(const Track& track, IdRemapTable& table, ItemIdAllocator& allocator,
                         std::optional<ModelError>& error) {
    for (const auto& lane : track.take_lanes()) {
        if (error)
            return;
        error = allocate_owned(table, allocator, lane.id());
        for (const auto& take : lane.takes()) {
            if (error)
                return;
            error = allocate_owned(table, allocator, take.id());
        }
    }
}

// A take's identity is owned and remapped; its MediaRef::asset_id is an external
// reference fixed up the same way a clip's MediaRef is, so a remapped project's
// takes point at the remapped assets.
runtime::Result<TakeLane, ModelError>
rebuild_take_lane(const TakeLane& lane, const IdRemapTable& table, ExternalIdFixup external) {
    std::vector<Take> takes;
    takes.reserve(lane.takes().size());
    for (const auto& take : lane.takes()) {
        auto fixed = external.apply(take.media().asset_id);
        if (!fixed)
            return fail<TakeLane>(fixed.error().code, fixed.error().item,
                                  fixed.error().related_item);
        MediaRef media = take.media();
        media.asset_id = fixed.value();
        auto rebuilt =
            Take::create(*table.find(take.id()), media, take.placement_start(), take.sample_rate());
        if (!rebuilt)
            return fail<TakeLane>(rebuilt.error().code, rebuilt.error().item,
                                  rebuilt.error().related_item);
        takes.push_back(std::move(rebuilt).value());
    }
    std::vector<TakeCompSegment> comp(lane.comp_segments().begin(), lane.comp_segments().end());
    for (auto& segment : comp)
        segment.take_id = *table.find(segment.take_id);
    return TakeLane::create(*table.find(lane.id()), lane.name(), std::move(takes),
                            std::move(comp));
}

runtime::Result<Track, ModelError> rebuild_track(const Track& track, const IdRemapTable& table,
                                                 ExternalIdFixup external) {
    std::vector<DevicePlacement> device_chain;
    device_chain.reserve(track.device_chain().size());
    for (const auto& device : track.device_chain())
        device_chain.push_back({*table.find(device.id)});
    std::vector<Clip> clips;
    clips.reserve(track.clips().size());
    for (const auto& clip : track.clips()) {
        auto rebuilt = rebuild_clip(clip, table, external);
        if (!rebuilt)
            return fail<Track>(rebuilt.error().code, rebuilt.error().item,
                               rebuilt.error().related_item);
        clips.push_back(std::move(rebuilt).value());
    }
    std::vector<AutomationLane> automation_lanes;
    automation_lanes.reserve(track.automation_lanes().size());
    for (const auto& lane : track.automation_lanes()) {
        auto rebuilt = detail::remap_attached_automation_lane(lane, table);
        if (!rebuilt)
            return fail<Track>(rebuilt.error().code, rebuilt.error().item,
                               rebuilt.error().related_item);
        automation_lanes.push_back(std::move(rebuilt).value());
    }
    std::vector<TakeLane> take_lanes;
    take_lanes.reserve(track.take_lanes().size());
    for (const auto& lane : track.take_lanes()) {
        auto rebuilt = rebuild_take_lane(lane, table, external);
        if (!rebuilt)
            return fail<Track>(rebuilt.error().code, rebuilt.error().item,
                               rebuilt.error().related_item);
        take_lanes.push_back(std::move(rebuilt).value());
    }
    auto freeze = track.freeze();
    if (freeze) {
        auto fixed = external.apply(freeze->media.asset_id);
        if (!fixed)
            return fail<Track>(fixed.error().code, fixed.error().item, fixed.error().related_item);
        freeze->media.asset_id = fixed.value();
    }
    return Track::create(
        TrackInput{.id = *table.find(track.id()),
                   .name = track.name(),
                   .clips = std::move(clips),
                   .device_chain = std::move(device_chain),
                   .automation_lanes = std::move(automation_lanes),
                   .take_lanes = std::move(take_lanes),
                   .record_armed = track.record_armed(),
                   .active_take_lane_id = track.active_take_lane_id().valid()
                                              ? *table.find(track.active_take_lane_id())
                                              : ItemId{},
                   .freeze = std::move(freeze)});
}

runtime::Result<Sequence, ModelError>
rebuild_sequence(const Sequence& sequence, const IdRemapTable& table, ExternalIdFixup external) {
    std::vector<Track> tracks;
    tracks.reserve(sequence.tracks().size());
    for (const auto& track : sequence.tracks()) {
        auto rebuilt = rebuild_track(track, table, external);
        if (!rebuilt)
            return fail<Sequence>(rebuilt.error().code, rebuilt.error().item,
                                  rebuilt.error().related_item);
        tracks.push_back(std::move(rebuilt).value());
    }
    return Sequence::create(*table.find(sequence.id()), sequence.name(), sequence.duration(),
                            sequence.absolute_duration(), std::move(tracks));
}

} // namespace

runtime::Result<RemappedClip, ModelError> remap_ids(const Clip& clip, ItemIdAllocator& allocator,
                                                    ExternalIdFixup external) {
    if (const auto error = preflight(clip))
        return fail<RemappedClip>(error->code, error->item, error->related_item);
    auto working = allocator;
    IdRemapTable table;
    std::optional<ModelError> error;
    allocate_clip_owned(clip, table, working, error);
    if (error)
        return fail<RemappedClip>(error->code, error->item, error->related_item);
    if (const auto table_error = finish_table(table))
        return fail<RemappedClip>(table_error->code, table_error->item, table_error->related_item);
    auto rebuilt = rebuild_clip(clip, table, external);
    if (!rebuilt)
        return fail<RemappedClip>(rebuilt.error().code, rebuilt.error().item,
                                  rebuilt.error().related_item);
    allocator = working;
    return runtime::Result<RemappedClip, ModelError>(
        runtime::Ok(RemappedClip{std::move(rebuilt).value(), std::move(table)}));
}

runtime::Result<RemappedTrack, ModelError> remap_ids(const Track& track, ItemIdAllocator& allocator,
                                                     ExternalIdFixup external) {
    if (const auto error = preflight(track))
        return fail<RemappedTrack>(error->code, error->item, error->related_item);
    auto working = allocator;
    IdRemapTable table;
    std::optional<ModelError> error = allocate_owned(table, working, track.id());
    for (const auto& device : track.device_chain()) {
        if (!error)
            error = allocate_owned(table, working, device.id);
    }
    for (const auto& clip : track.clips())
        allocate_clip_owned(clip, table, working, error);
    for (const auto& lane : track.automation_lanes())
        allocate_automation_owned(lane, table, working, error);
    allocate_take_owned(track, table, working, error);
    if (error)
        return fail<RemappedTrack>(error->code, error->item, error->related_item);
    if (const auto table_error = finish_table(table))
        return fail<RemappedTrack>(table_error->code, table_error->item, table_error->related_item);
    auto rebuilt = rebuild_track(track, table, external);
    if (!rebuilt)
        return fail<RemappedTrack>(rebuilt.error().code, rebuilt.error().item,
                                   rebuilt.error().related_item);
    allocator = working;
    return runtime::Result<RemappedTrack, ModelError>(
        runtime::Ok(RemappedTrack{std::move(rebuilt).value(), std::move(table)}));
}

runtime::Result<RemappedSequence, ModelError>
remap_ids(const Sequence& sequence, ItemIdAllocator& allocator, ExternalIdFixup external) {
    if (const auto error = preflight(sequence))
        return fail<RemappedSequence>(error->code, error->item, error->related_item);
    auto working = allocator;
    IdRemapTable table;
    std::optional<ModelError> error = allocate_owned(table, working, sequence.id());
    for (const auto& track : sequence.tracks()) {
        if (!error)
            error = allocate_owned(table, working, track.id());
        for (const auto& device : track.device_chain()) {
            if (!error)
                error = allocate_owned(table, working, device.id);
        }
        for (const auto& clip : track.clips())
            allocate_clip_owned(clip, table, working, error);
        for (const auto& lane : track.automation_lanes())
            allocate_automation_owned(lane, table, working, error);
        allocate_take_owned(track, table, working, error);
    }
    if (error)
        return fail<RemappedSequence>(error->code, error->item, error->related_item);
    if (const auto table_error = finish_table(table))
        return fail<RemappedSequence>(table_error->code, table_error->item,
                                      table_error->related_item);
    auto rebuilt = rebuild_sequence(sequence, table, external);
    if (!rebuilt)
        return fail<RemappedSequence>(rebuilt.error().code, rebuilt.error().item,
                                      rebuilt.error().related_item);
    allocator = working;
    return runtime::Result<RemappedSequence, ModelError>(
        runtime::Ok(RemappedSequence{std::move(rebuilt).value(), std::move(table)}));
}

runtime::Result<RemappedProject, ModelError> remap_ids(const Project& project,
                                                       std::uint64_t first_id) {
    for (const auto& sequence : project.sequences())
        for (const auto& track : sequence.tracks())
            for (const auto& clip : track.clips())
                if (std::holds_alternative<OpaqueContent>(clip.content()))
                    return fail<RemappedProject>(ModelErrorCode::OpaqueContentCannotRemap,
                                                 clip.id());
    ItemIdAllocator allocator(first_id);
    IdRemapTable table;
    const auto identities = detail::ProjectStateAccess::identity_entries(project);
    std::optional<ModelError> error;
    for (const auto& identity : identities) {
        if (!error)
            error = allocate_owned(table, allocator, identity.item);
    }
    if (error)
        return fail<RemappedProject>(error->code, error->item, error->related_item);
    if (const auto table_error = finish_table(table))
        return fail<RemappedProject>(table_error->code, table_error->item,
                                     table_error->related_item);

    std::vector<MediaAsset> assets;
    assets.reserve(project.assets().size());
    for (const auto& asset : project.assets()) {
        auto copy = asset;
        copy.id = *table.find(asset.id);
        assets.push_back(std::move(copy));
    }
    struct Context {
        const IdRemapTable* table;
    } context{&table};
    const ExternalIdFixup internal{
        &context, [](void* raw, ItemId id) noexcept -> runtime::Result<ItemId, ModelError> {
            const auto* ctx = static_cast<Context*>(raw);
            const auto mapped = ctx->table->find(id);
            return mapped ? runtime::Result<ItemId, ModelError>(runtime::Ok(*mapped))
                          : fail<ItemId>(ModelErrorCode::MissingAsset, {}, id);
        }};
    std::vector<Sequence> sequences;
    sequences.reserve(project.sequences().size());
    for (const auto& sequence : project.sequences()) {
        auto rebuilt = rebuild_sequence(sequence, table, internal);
        if (!rebuilt)
            return fail<RemappedProject>(rebuilt.error().code, rebuilt.error().item,
                                         rebuilt.error().related_item);
        sequences.push_back(std::move(rebuilt).value());
    }
    auto rebuilt = Project::create(
        ProjectInput{*table.find(project.id()), project.name(), allocator.next_value(),
                     *table.find(project.root_sequence_id()), std::move(assets),
                     std::move(sequences), project.tempo_map(), project.meter_map()});
    if (!rebuilt)
        return fail<RemappedProject>(rebuilt.error().code, rebuilt.error().item,
                                     rebuilt.error().related_item);

    std::vector<detail::IdentityRecord> remapped_identities;
    remapped_identities.reserve(identities.size());
    for (const auto& identity : identities) {
        auto location = identity.location;
        const auto remap_owner = [&](ItemId& owner) {
            if (!owner.valid())
                return true;
            const auto mapped = table.find(owner);
            if (!mapped)
                return false;
            owner = *mapped;
            return true;
        };
        const auto mapped_item = table.find(identity.item);
        if (!mapped_item || !remap_owner(location.parent_id) ||
            !remap_owner(location.sequence_id) || !remap_owner(location.track_id) ||
            !remap_owner(location.clip_id))
            return fail<RemappedProject>(ModelErrorCode::InvalidSchemaIdentity, identity.item);
        remapped_identities.push_back({*mapped_item, location});
    }
    auto restored = detail::ProjectStateAccess::restore_identities(std::move(rebuilt).value(),
                                                                   std::move(remapped_identities));
    if (!restored)
        return fail<RemappedProject>(restored.error().code, restored.error().item,
                                     restored.error().related_item);
    return runtime::Result<RemappedProject, ModelError>(
        runtime::Ok(RemappedProject{std::move(restored).value(), std::move(table)}));
}

} // namespace pulp::timeline
