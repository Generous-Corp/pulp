#include <pulp/timeline/model.hpp>

#include "automation_document_internal.hpp"
#include "modulation_document_internal.hpp"
#include "owned_identity_traversal.hpp"
#include "project_state_access.hpp"
#include "track_input_access.hpp"

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

// Adapts the three canonical traversals to one name, so the two passes every
// level runs — collect the owned ids for preflight, then issue a fresh id for
// each — are written once and each level gets exactly the enumeration the
// traversal defines. Only the identity is read here, so the clip walk needs no
// track coordinate.
template <typename Visitor>
void visit_owned(const Clip& clip, Visitor&& visitor) {
    detail::visit_clip_owned_identities(clip, {}, visitor);
}

template <typename Visitor>
void visit_owned(const Track& track, Visitor&& visitor) {
    detail::visit_track_owned_identities(track, visitor);
}

template <typename Visitor>
void visit_owned(const Sequence& sequence, Visitor&& visitor) {
    detail::visit_sequence_owned_identities(sequence, visitor);
}

template <typename Owner>
std::vector<ItemId> owned_ids(const Owner& owner) {
    std::vector<ItemId> ids;
    visit_owned(owner, [&](const detail::ModelOwnedIdentity& identity) {
        ids.push_back(identity.id);
    });
    return ids;
}

// The first allocation failure is kept and the rest of the walk is skipped, so
// neither the table nor the allocator advances past it.
template <typename Owner>
std::optional<ModelError> allocate_owned_subtree(IdRemapTable& table, ItemIdAllocator& allocator,
                                                 const Owner& owner) {
    std::optional<ModelError> error;
    visit_owned(owner, [&](const detail::ModelOwnedIdentity& identity) {
        if (!error)
            error = allocate_owned(table, allocator, identity.id);
    });
    return error;
}

// A clip can only be remapped when its content's identity/reference shape is
// legible. Answering this per alternative in one place keeps the preflight
// walks, which each scan a different level of the tree, from drifting apart.
std::optional<ModelErrorCode> remap_rejection(const ClipContent& content) noexcept {
    return std::visit(
        ClipContentCases{
            [](const EmptyContent&) { return std::optional<ModelErrorCode>{}; },
            [](const MediaRef&) { return std::optional<ModelErrorCode>{}; },
            [](const MidiContent&) { return std::optional<ModelErrorCode>{}; },
            [](const RegisteredContent&) { return std::optional<ModelErrorCode>{}; },
            [](const OpaqueContent&) {
                return std::optional<ModelErrorCode>{ModelErrorCode::OpaqueContentCannotRemap};
            },
            [](const SequenceRef&) { return std::optional<ModelErrorCode>{}; },
        },
        content);
}

std::optional<ModelError> preflight(const Clip& clip) {
    if (const auto rejected = remap_rejection(clip.content()))
        return ModelError{*rejected, clip.id(), {}};
    return validate_owned_ids(owned_ids(clip));
}

std::optional<ModelError> preflight(const Track& track) {
    for (const auto& clip : track.clips())
        if (const auto rejected = remap_rejection(clip.content()))
            return ModelError{*rejected, clip.id(), {}};
    return validate_owned_ids(owned_ids(track));
}

std::optional<ModelError> preflight(const Sequence& sequence) {
    for (const auto& track : sequence.tracks())
        for (const auto& clip : track.clips())
            if (const auto rejected = remap_rejection(clip.content()))
                return ModelError{*rejected, clip.id(), {}};
    return validate_owned_ids(owned_ids(sequence));
}

runtime::Result<Clip, ModelError> rebuild_clip(const Clip& clip, const IdRemapTable& table,
                                               RemapIdFixups fixups) {
    // Every owned id collected by the canonical identity traversal is rewritten
    // here, and every external reference is fixed up.
    ClipContent content = clip.content();
    std::optional<ModelError> content_error;
    std::visit(ClipContentCases{
                   [](const EmptyContent&) {},
                   [&](MediaRef& media) {
                       auto fixed = fixups.asset.apply(media.asset_id);
                       if (!fixed)
                           content_error = fixed.error();
                       else
                           media.asset_id = fixed.value();
                   },
                   [&](MidiContent& old_notes) {
                       std::vector<NoteEvent> notes(old_notes.notes().begin(),
                                                    old_notes.notes().end());
                       for (auto& note : notes)
                           note.id = *table.find(note.id);
                       // Modifiers reference notes by id, so they follow the
                       // same rewrite; the seed is copy-invariant identity, not
                       // an id, and is carried across unchanged.
                       std::vector<NoteModifier> modifiers(old_notes.modifiers().begin(),
                                                           old_notes.modifiers().end());
                       for (auto& modifier : modifiers)
                           modifier.note_id = *table.find(modifier.note_id);
                       // Lanes and their points own identities of their own, so
                       // both are rewritten; a lane's address is a wire
                       // coordinate rather than a document identity and is
                       // carried across unchanged.
                       std::vector<MidiExpressionLane> lanes(old_notes.lanes().begin(),
                                                             old_notes.lanes().end());
                       for (auto& lane : lanes) {
                           lane.id = *table.find(lane.id);
                           for (auto& point : lane.points)
                               point.id = *table.find(point.id);
                       }
                       auto rebuilt = MidiContent::create(std::move(notes), std::move(modifiers),
                                                          old_notes.modifier_seed(),
                                                          std::move(lanes));
                       if (!rebuilt)
                           content_error = rebuilt.error();
                       else
                           old_notes = std::move(rebuilt).value();
                   },
                   // Registered payloads own no ItemIds by construction, and
                   // opaque content never reaches here — preflight refuses it.
                   [](RegisteredContent&) {},
                   [](OpaqueContent&) {},
                   [&](SequenceRef& reference) {
                       auto fixed = fixups.sequence.apply(reference.sequence_id);
                       if (!fixed)
                           content_error = fixed.error();
                       else
                           reference.sequence_id = fixed.value();
                   },
               },
               content);
    if (content_error)
        return fail<Clip>(content_error->code, content_error->item, content_error->related_item);
    if (clip.time_anchor() == ClipTimeAnchor::Musical)
        return Clip::create(*table.find(clip.id()), clip.start(), clip.duration(),
                            std::move(content), clip.playback_properties(), clip.time_conform());
    return Clip::create_absolute(*table.find(clip.id()), clip.absolute_start(),
                                 clip.absolute_duration_samples(), clip.absolute_sample_rate(),
                                 std::move(content), clip.playback_properties(),
                                 clip.time_conform());
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
    return TakeLane::create(*table.find(lane.id()), lane.name(), std::move(takes), std::move(comp));
}

runtime::Result<Track, ModelError> rebuild_track(const Track& track, const IdRemapTable& table,
                                                 RemapIdFixups fixups) {
    std::vector<DevicePlacement> device_chain;
    device_chain.reserve(track.device_chain().size());
    for (const auto& device : track.device_chain())
        device_chain.push_back({*table.find(device.id)});
    std::vector<Clip> clips;
    clips.reserve(track.clips().size());
    for (const auto& clip : track.clips()) {
        auto rebuilt = rebuild_clip(clip, table, fixups);
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
    std::vector<Modulator> modulators(track.modulators().begin(), track.modulators().end());
    for (auto& modulator : modulators)
        modulator.id = *table.find(modulator.id);
    std::vector<MacroControl> macros(track.macros().begin(), track.macros().end());
    for (auto& macro : macros)
        macro.id = *table.find(macro.id);
    std::vector<ModulationRoute> modulation_routes;
    modulation_routes.reserve(track.modulation_routes().size());
    for (const auto& route : track.modulation_routes()) {
        auto rebuilt = detail::remap_attached_modulation_route(route, table);
        if (!rebuilt)
            return fail<Track>(rebuilt.error().code, rebuilt.error().item,
                               rebuilt.error().related_item);
        modulation_routes.push_back(std::move(rebuilt).value());
    }
    std::vector<TakeLane> take_lanes;
    take_lanes.reserve(track.take_lanes().size());
    for (const auto& lane : track.take_lanes()) {
        auto rebuilt = rebuild_take_lane(lane, table, fixups.asset);
        if (!rebuilt)
            return fail<Track>(rebuilt.error().code, rebuilt.error().item,
                               rebuilt.error().related_item);
        take_lanes.push_back(std::move(rebuilt).value());
    }
    auto freeze = track.freeze();
    if (freeze) {
        auto fixed = fixups.asset.apply(freeze->media.asset_id);
        if (!fixed)
            return fail<Track>(fixed.error().code, fixed.error().item, fixed.error().related_item);
        freeze->media.asset_id = fixed.value();
    }
    // A remap rewrites identities and carries authored value state across
    // unchanged, so it names only the identity-bearing fields over a copy of
    // the source input. Enumerating the whole struct instead would silently
    // reset any authored field the list forgot to a default.
    auto input = detail::track_input_of(track);
    input.id = *table.find(track.id());
    input.clips = std::move(clips);
    input.device_chain = std::move(device_chain);
    input.automation_lanes = std::move(automation_lanes);
    input.modulators = std::move(modulators);
    input.macros = std::move(macros);
    input.modulation_routes = std::move(modulation_routes);
    input.take_lanes = std::move(take_lanes);
    input.active_take_lane_id =
        track.active_take_lane_id().valid() ? *table.find(track.active_take_lane_id()) : ItemId{};
    input.freeze = std::move(freeze);
    return Track::create(std::move(input));
}

runtime::Result<Sequence, ModelError>
rebuild_sequence(const Sequence& sequence, const IdRemapTable& table, RemapIdFixups fixups) {
    std::vector<Track> tracks;
    tracks.reserve(sequence.tracks().size());
    for (const auto& track : sequence.tracks()) {
        auto rebuilt = rebuild_track(track, table, fixups);
        if (!rebuilt)
            return fail<Sequence>(rebuilt.error().code, rebuilt.error().item,
                                  rebuilt.error().related_item);
        tracks.push_back(std::move(rebuilt).value());
    }
    // Authored order names the same identities the track list does, so it is
    // remapped alongside them. Dropping it would silently reset the arrangement
    // to identity order every time a sequence is copied or imported.
    std::vector<ItemId> track_order;
    track_order.reserve(sequence.track_order().size());
    for (const auto& id : sequence.track_order())
        track_order.push_back(*table.find(id));
    std::vector<SequenceMarker> markers(sequence.markers().begin(), sequence.markers().end());
    for (auto& marker : markers)
        marker.id = *table.find(marker.id);
    std::vector<SequenceRegion> regions(sequence.regions().begin(), sequence.regions().end());
    for (auto& region : regions)
        region.id = *table.find(region.id);
    std::vector<Scene> scenes;
    scenes.reserve(sequence.scenes().size());
    for (const auto& scene : sequence.scenes()) {
        std::vector<Slot> slots(scene.slots.begin(), scene.slots.end());
        for (auto& slot : slots) {
            slot.id = *table.find(slot.id);
            if (slot.clip_id.valid())
                slot.clip_id = *table.find(slot.clip_id);
            for (auto& action : slot.follow.choices)
                if (action.kind == FollowActionKind::Jump && action.target.valid())
                    action.target = *table.find(action.target);
        }
        scenes.push_back(Scene{*table.find(scene.id), scene.name, std::move(slots)});
    }
    return Sequence::create(SequenceInput{
        .id = *table.find(sequence.id()),
        .name = sequence.name(),
        .musical_duration = sequence.duration(),
        .absolute_duration = sequence.absolute_duration(),
        .tracks = std::move(tracks),
        .markers = std::move(markers),
        .regions = std::move(regions),
        .chord_scale_lane = sequence.chord_scale_lane(),
        .groove = sequence.groove(),
        .scenes = std::move(scenes),
        .track_order = std::move(track_order),
    });
}

} // namespace

runtime::Result<RemappedClip, ModelError> remap_ids(const Clip& clip, ItemIdAllocator& allocator,
                                                    ExternalIdFixup external) {
    return remap_ids(clip, allocator, RemapIdFixups{external, {}});
}

runtime::Result<RemappedClip, ModelError> remap_ids(const Clip& clip, ItemIdAllocator& allocator,
                                                    RemapIdFixups fixups) {
    if (const auto error = preflight(clip))
        return fail<RemappedClip>(error->code, error->item, error->related_item);
    auto working = allocator;
    IdRemapTable table;
    if (const auto error = allocate_owned_subtree(table, working, clip))
        return fail<RemappedClip>(error->code, error->item, error->related_item);
    if (const auto table_error = finish_table(table))
        return fail<RemappedClip>(table_error->code, table_error->item, table_error->related_item);
    auto rebuilt = rebuild_clip(clip, table, fixups);
    if (!rebuilt)
        return fail<RemappedClip>(rebuilt.error().code, rebuilt.error().item,
                                  rebuilt.error().related_item);
    allocator = working;
    return runtime::Result<RemappedClip, ModelError>(
        runtime::Ok(RemappedClip{std::move(rebuilt).value(), std::move(table)}));
}

runtime::Result<RemappedTrack, ModelError> remap_ids(const Track& track, ItemIdAllocator& allocator,
                                                     ExternalIdFixup external) {
    return remap_ids(track, allocator, RemapIdFixups{external, {}});
}

runtime::Result<RemappedTrack, ModelError> remap_ids(const Track& track, ItemIdAllocator& allocator,
                                                     RemapIdFixups fixups) {
    if (const auto error = preflight(track))
        return fail<RemappedTrack>(error->code, error->item, error->related_item);
    auto working = allocator;
    IdRemapTable table;
    if (const auto error = allocate_owned_subtree(table, working, track))
        return fail<RemappedTrack>(error->code, error->item, error->related_item);
    if (const auto table_error = finish_table(table))
        return fail<RemappedTrack>(table_error->code, table_error->item, table_error->related_item);
    auto rebuilt = rebuild_track(track, table, fixups);
    if (!rebuilt)
        return fail<RemappedTrack>(rebuilt.error().code, rebuilt.error().item,
                                   rebuilt.error().related_item);
    allocator = working;
    return runtime::Result<RemappedTrack, ModelError>(
        runtime::Ok(RemappedTrack{std::move(rebuilt).value(), std::move(table)}));
}

runtime::Result<RemappedSequence, ModelError>
remap_ids(const Sequence& sequence, ItemIdAllocator& allocator, ExternalIdFixup external) {
    return remap_ids(sequence, allocator, RemapIdFixups{external, {}});
}

runtime::Result<RemappedSequence, ModelError>
remap_ids(const Sequence& sequence, ItemIdAllocator& allocator, RemapIdFixups fixups) {
    if (const auto error = preflight(sequence))
        return fail<RemappedSequence>(error->code, error->item, error->related_item);
    auto working = allocator;
    IdRemapTable table;
    if (const auto error = allocate_owned_subtree(table, working, sequence))
        return fail<RemappedSequence>(error->code, error->item, error->related_item);
    if (const auto table_error = finish_table(table))
        return fail<RemappedSequence>(table_error->code, table_error->item,
                                      table_error->related_item);
    auto rebuilt = rebuild_sequence(sequence, table, fixups);
    if (!rebuilt)
        return fail<RemappedSequence>(rebuilt.error().code, rebuilt.error().item,
                                      rebuilt.error().related_item);
    allocator = working;
    return runtime::Result<RemappedSequence, ModelError>(
        runtime::Ok(RemappedSequence{std::move(rebuilt).value(), std::move(table)}));
}

runtime::Result<RemappedSequence, ModelError>
remap_ids(const Sequence& sequence,
          std::span<const std::pair<ItemId, ItemId>> carried_ids,
          RemapIdFixups fixups) {
    if (const auto error = preflight(sequence))
        return fail<RemappedSequence>(error->code, error->item, error->related_item);
    auto expected = owned_ids(sequence);
    std::sort(expected.begin(), expected.end());
    if (carried_ids.size() != expected.size())
        return fail<RemappedSequence>(ModelErrorCode::InvalidIdentityTransition, sequence.id());
    IdRemapTable table;
    auto& entries = IdRemapBuilder::entries(table);
    entries.assign(carried_ids.begin(), carried_ids.end());
    std::vector<ItemId> mapped;
    mapped.reserve(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (entries[index].first != expected[index])
            return fail<RemappedSequence>(ModelErrorCode::InvalidIdentityTransition,
                                          entries[index].first, expected[index]);
        mapped.push_back(entries[index].second);
    }
    if (const auto error = validate_owned_ids(std::move(mapped)))
        return fail<RemappedSequence>(error->code, error->item, error->related_item);
    auto rebuilt = rebuild_sequence(sequence, table, fixups);
    if (!rebuilt)
        return fail<RemappedSequence>(rebuilt.error().code, rebuilt.error().item,
                                      rebuilt.error().related_item);
    return runtime::Ok(
        RemappedSequence{std::move(rebuilt).value(), std::move(table)});
}

runtime::Result<RemappedProject, ModelError> remap_ids(const Project& project,
                                                       std::uint64_t first_id) {
    for (const auto& sequence : project.sequences())
        for (const auto& track : sequence.tracks())
            for (const auto& clip : track.clips())
                if (const auto rejected = remap_rejection(clip.content()))
                    return fail<RemappedProject>(*rejected, clip.id());
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
    const ExternalIdFixup asset_internal{
        &context, [](void* raw, ItemId id) noexcept -> runtime::Result<ItemId, ModelError> {
            const auto* ctx = static_cast<Context*>(raw);
            const auto mapped = ctx->table->find(id);
            return mapped ? runtime::Result<ItemId, ModelError>(runtime::Ok(*mapped))
                          : fail<ItemId>(ModelErrorCode::MissingAsset, {}, id);
        }};
    const ExternalIdFixup sequence_internal{
        &context, [](void* raw, ItemId id) noexcept -> runtime::Result<ItemId, ModelError> {
            const auto* ctx = static_cast<Context*>(raw);
            const auto mapped = ctx->table->find(id);
            return mapped ? runtime::Result<ItemId, ModelError>(runtime::Ok(*mapped))
                          : fail<ItemId>(ModelErrorCode::MissingSequenceReference, {}, id);
        }};
    std::vector<Sequence> sequences;
    sequences.reserve(project.sequences().size());
    for (const auto& sequence : project.sequences()) {
        auto rebuilt =
            rebuild_sequence(sequence, table, RemapIdFixups{asset_internal, sequence_internal});
        if (!rebuilt)
            return fail<RemappedProject>(rebuilt.error().code, rebuilt.error().item,
                                         rebuilt.error().related_item);
        sequences.push_back(std::move(rebuilt).value());
    }
    auto rebuilt =
        Project::create(ProjectInput{.id = *table.find(project.id()),
                                     .name = project.name(),
                                     .next_item_id = allocator.next_value(),
                                     .root_sequence_id = *table.find(project.root_sequence_id()),
                                     .assets = std::move(assets),
                                     .sequences = std::move(sequences),
                                     .tempo_map = project.tempo_map(),
                                     .meter_map = project.meter_map(),
                                     .session_start = project.session_start()});
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
