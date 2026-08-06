#include <pulp/timeline/asset_path.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_json.hpp>

#include "asset_validation.hpp"
#include "identity_directory.hpp"
#include "identity_transition.hpp"
#include "media_reference_validation.hpp"
#include "owned_identity_traversal.hpp"
#include "project_state_access.hpp"
#include "sequence_graph_validation.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

namespace pulp::timeline {

namespace {

std::atomic<std::uint64_t> g_next_sequence_compile_structure_token{1};

std::uint64_t next_sequence_compile_structure_token() noexcept {
    const auto token =
        g_next_sequence_compile_structure_token.fetch_add(1, std::memory_order_relaxed);
    // Exhausting 64 bits of process-local structural publications is not a
    // recoverable operating mode. Keep zero reserved as the invalid token.
    return token == 0
               ? g_next_sequence_compile_structure_token.fetch_add(1, std::memory_order_relaxed)
               : token;
}

template <typename T>
runtime::Result<T, ModelError> fail(ModelErrorCode code, ItemId item = {}, ItemId related = {}) {
    return runtime::Result<T, ModelError>(runtime::Err(ModelError{code, item, related}));
}

bool positive_range(std::int64_t start, std::int64_t duration) noexcept {
    return duration > 0 && start <= std::numeric_limits<std::int64_t>::max() - duration;
}

template <typename T, typename IdFn>
std::optional<ItemId> first_duplicate(const std::vector<T>& values, IdFn&& id_of) {
    std::vector<ItemId> ids;
    ids.reserve(values.size());
    for (const auto& value : values)
        ids.push_back(id_of(value));
    std::sort(ids.begin(), ids.end());
    const auto duplicate = std::adjacent_find(ids.begin(), ids.end());
    return duplicate == ids.end() ? std::nullopt : std::optional<ItemId>(*duplicate);
}

bool valid_storage_policy(AssetStoragePolicy policy) noexcept {
    switch (policy) {
    case AssetStoragePolicy::External:
    case AssetStoragePolicy::Embedded:
    case AssetStoragePolicy::PreferEmbedded:
        return true;
    }
    return false;
}

bool valid_locator_kind(AssetLocatorKind kind) noexcept {
    switch (kind) {
    case AssetLocatorKind::PackageRelative:
    case AssetLocatorKind::ExternalUri:
        return true;
    }
    return false;
}

bool valid_locator(const AssetLocator& locator) noexcept {
    if (!valid_locator_kind(locator.kind) || locator.hint.empty())
        return false;
    return locator.kind != AssetLocatorKind::PackageRelative ||
           package_relative_path_is_lexically_safe(locator.hint);
}

// Validates a media asset and canonicalizes its representation order. Shared by
// project construction and asset-append so both paths enforce the identical
// sealed-identity invariant: an asset with an invalid or empty ContentHash is
// rejected and never enters the document. Returns nullopt on success.
std::optional<ModelError> validate_media_asset(MediaAsset& asset) {
    if (!asset.id.valid())
        return ModelError{ModelErrorCode::InvalidItemId, asset.id, {}};
    if (!asset.sample_rate.valid())
        return ModelError{ModelErrorCode::InvalidSampleRate, asset.id, {}};
    asset.sample_rate = asset.sample_rate.normalized();
    if (!asset.content_hash.valid())
        return ModelError{ModelErrorCode::InvalidContentHash, asset.id, {}};
    if (!valid_storage_policy(asset.storage_policy))
        return ModelError{ModelErrorCode::InvalidAssetStoragePolicy, asset.id, {}};
    for (const auto& locator : asset.locators)
        if (!valid_locator(locator))
            return ModelError{ModelErrorCode::InvalidAssetLocator, asset.id, {}};
    std::vector<std::string_view> roles;
    roles.reserve(asset.representations.size());
    std::vector<ContentHash> hashes;
    hashes.reserve(asset.representations.size() + 1);
    hashes.push_back(asset.content_hash);
    for (const auto& representation : asset.representations) {
        if (!representation.content_hash.valid())
            return ModelError{ModelErrorCode::InvalidContentHash, asset.id, {}};
        if (!valid_storage_policy(representation.storage_policy))
            return ModelError{ModelErrorCode::InvalidAssetStoragePolicy, asset.id, {}};
        if (representation.role.empty())
            return ModelError{ModelErrorCode::InvalidAssetLocator, asset.id, {}};
        roles.push_back(representation.role);
        hashes.push_back(representation.content_hash);
        for (const auto& locator : representation.locators)
            if (!valid_locator(locator))
                return ModelError{ModelErrorCode::InvalidAssetLocator, asset.id, {}};
    }
    std::sort(roles.begin(), roles.end());
    if (std::adjacent_find(roles.begin(), roles.end()) != roles.end())
        return ModelError{ModelErrorCode::DuplicateAssetRepresentation, asset.id, {}};
    std::sort(hashes.begin(), hashes.end());
    if (std::adjacent_find(hashes.begin(), hashes.end()) != hashes.end())
        return ModelError{ModelErrorCode::DuplicateAssetRepresentation, asset.id, {}};
    std::sort(asset.representations.begin(), asset.representations.end(),
              [](const AssetRepresentation& lhs, const AssetRepresentation& rhs) {
                  return lhs.role < rhs.role;
              });
    if (asset.loop_info && !detail::validate_and_canonicalize(*asset.loop_info, asset.frame_count))
        return ModelError{ModelErrorCode::InvalidAudioLoopInfo, asset.id, {}};
    return std::nullopt;
}

// Applies Insert / Deactivate / Reactivate identity mutations to a directory
// copy. Shared by sequence replacement and asset mutation so identity
// transitions have one enforcement path. Returns nullopt on success.
std::optional<ModelError> apply_identity_mutations(detail::IdentityDirectory& identities,
                                                   std::span<const IdentityMutation> mutations) {
    for (const auto& change : mutations) {
        if (!change.item.valid())
            return ModelError{ModelErrorCode::InvalidItemId, change.item, {}};
        const auto existing = identities.locate(change.item);
        switch (change.mutation) {
        case IdentityMutationKind::Insert: {
            if (existing)
                return ModelError{ModelErrorCode::IdentityConflict, change.item, {}};
            auto location = change.location;
            location.active = true;
            identities.insert(change.item, location);
            break;
        }
        case IdentityMutationKind::Deactivate: {
            if (!existing || !existing->active || !existing->has_same_owner(change.location))
                return ModelError{ModelErrorCode::InvalidIdentityTransition, change.item, {}};
            auto location = *existing;
            location.active = false;
            identities.replace(change.item, location);
            break;
        }
        case IdentityMutationKind::Reactivate: {
            auto location = detail::reactivated_location(existing, change.location);
            if (!location)
                return ModelError{ModelErrorCode::InvalidIdentityTransition, change.item, {}};
            identities.replace(change.item, *location);
            break;
        }
        }
    }
    return std::nullopt;
}

template <typename Visitor>
void visit_project_identities(const ProjectInput& input, Visitor&& visit) {
    const auto location = [&](ItemKind kind, ItemId sequence = {}, ItemId track = {},
                              ItemId clip = {}, ItemId lane = {}) {
        return ItemLocation{
            kind,     immediate_parent_id(kind, input.id, sequence, track, clip, lane),
            sequence, track,
            clip,     true};
    };
    visit(input.id, location(ItemKind::Project));
    for (const auto& asset : input.assets)
        visit(asset.id, location(ItemKind::Asset));
    for (const auto& sequence : input.sequences)
        detail::visit_sequence_owned_identities(
            sequence, [&](const detail::ModelOwnedIdentity& identity) {
                visit(identity.id, location(identity.kind, sequence.id(), identity.track,
                                            identity.clip, identity.lane));
            });
}

} // namespace

bool SchemaIdentity::valid() const noexcept {
    if (version == 0 || type_name.empty() || type_name.size() > 128)
        return false;
    bool segment_start = true;
    bool saw_dot = false;
    for (const auto raw : type_name) {
        const auto value = static_cast<unsigned char>(raw);
        if (value == '.') {
            if (segment_start)
                return false;
            segment_start = true;
            saw_dot = true;
            continue;
        }
        const bool lower = value >= 'a' && value <= 'z';
        const bool digit = value >= '0' && value <= '9';
        if (segment_start && !lower)
            return false;
        if (!segment_start && !lower && !digit && value != '_')
            return false;
        segment_start = false;
    }
    return saw_dot && !segment_start;
}

runtime::Result<ItemId, ModelError> ItemIdAllocator::allocate() noexcept {
    if (next_ == 0 || next_ == std::numeric_limits<std::uint64_t>::max())
        return fail<ItemId>(ModelErrorCode::ItemIdExhausted);
    const ItemId id{next_};
    ++next_;
    return runtime::Result<ItemId, ModelError>(runtime::Ok(id));
}

runtime::Result<MidiContent, ModelError> MidiContent::create(std::vector<NoteEvent> notes) {
    return create(std::move(notes), {}, 0, {});
}

runtime::Result<MidiContent, ModelError> MidiContent::create(std::vector<NoteEvent> notes,
                                                             std::vector<NoteModifier> modifiers,
                                                             std::uint64_t modifier_seed) {
    return create(std::move(notes), std::move(modifiers), modifier_seed, {});
}

runtime::Result<MidiContent, ModelError>
MidiContent::create(std::vector<NoteEvent> notes, std::vector<NoteModifier> modifiers,
                    std::uint64_t modifier_seed, std::vector<MidiExpressionLane> lanes) {
    for (const auto& note : notes) {
        if (!note.id.valid())
            return fail<MidiContent>(ModelErrorCode::InvalidItemId, note.id);
        if (!positive_range(note.start.value, note.duration.value) || note.pitch > 127 ||
            note.channel > 15)
            return fail<MidiContent>(ModelErrorCode::InvalidNote, note.id);
    }
    std::vector<ItemId> note_ids;
    note_ids.reserve(notes.size());
    for (const auto& note : notes)
        note_ids.push_back(note.id);
    std::sort(note_ids.begin(), note_ids.end());
    if (const auto duplicate = std::adjacent_find(note_ids.begin(), note_ids.end());
        duplicate != note_ids.end())
        return fail<MidiContent>(ModelErrorCode::DuplicateItemId, *duplicate);
    std::sort(notes.begin(), notes.end(), [](const NoteEvent& lhs, const NoteEvent& rhs) {
        return std::pair(lhs.start.value, lhs.id.value) < std::pair(rhs.start.value, rhs.id.value);
    });
    for (const auto& modifier : modifiers) {
        if (!modifier.note_id.valid())
            return fail<MidiContent>(ModelErrorCode::InvalidItemId, modifier.note_id);
        // A neutral entry describes a note that already plays that way, so
        // admitting it would give one document two byte encodings.
        if (!note_modifier_well_formed(modifier) || note_modifier_is_neutral(modifier))
            return fail<MidiContent>(ModelErrorCode::InvalidNoteModifier, modifier.note_id);
        if (!std::binary_search(note_ids.begin(), note_ids.end(), modifier.note_id))
            return fail<MidiContent>(ModelErrorCode::MissingItem, modifier.note_id);
    }
    if (const auto duplicate =
            first_duplicate(modifiers, [](const NoteModifier& entry) { return entry.note_id; }))
        return fail<MidiContent>(ModelErrorCode::DuplicateItemId, *duplicate);
    std::sort(modifiers.begin(), modifiers.end(),
              [](const NoteModifier& lhs, const NoteModifier& rhs) {
                  return lhs.note_id.value < rhs.note_id.value;
              });
    // Lane and point identities share the document's one ItemId space with the
    // notes above them, so `owned` accumulates all three: an identity reused
    // between a note and a lane point would make a later remap ambiguous about
    // which object it just renamed.
    auto owned = std::move(note_ids);
    for (const auto& lane : lanes) {
        if (!lane.id.valid())
            return fail<MidiContent>(ModelErrorCode::InvalidItemId, lane.id);
        if (!midi_lane_address_well_formed(lane.address))
            return fail<MidiContent>(ModelErrorCode::InvalidMidiLane, lane.id);
        owned.push_back(lane.id);
        for (const auto& point : lane.points) {
            if (!point.id.valid())
                return fail<MidiContent>(ModelErrorCode::InvalidItemId, point.id);
            if (point.position.value < 0)
                return fail<MidiContent>(ModelErrorCode::InvalidMidiLane, point.id);
            owned.push_back(point.id);
        }
    }
    std::sort(owned.begin(), owned.end());
    if (const auto duplicate = std::adjacent_find(owned.begin(), owned.end());
        duplicate != owned.end())
        return fail<MidiContent>(ModelErrorCode::DuplicateItemId, *duplicate);
    for (auto& lane : lanes)
        std::sort(lane.points.begin(), lane.points.end(),
                  [](const MidiLanePoint& lhs, const MidiLanePoint& rhs) {
                      return std::pair(lhs.position.value, lhs.id.value) <
                             std::pair(rhs.position.value, rhs.id.value);
                  });
    std::sort(lanes.begin(), lanes.end(),
              [](const MidiExpressionLane& lhs, const MidiExpressionLane& rhs) {
                  return std::pair(lhs.address, lhs.id.value) <
                         std::pair(rhs.address, rhs.id.value);
              });
    // Addresses are compared after the sort, so the duplicate reported is the
    // second lane claiming a stream rather than whichever was supplied later.
    for (std::size_t index = 1; index < lanes.size(); ++index)
        if (lanes[index].address == lanes[index - 1].address)
            return fail<MidiContent>(ModelErrorCode::DuplicateMidiLaneAddress, lanes[index].id,
                                     lanes[index - 1].id);
    auto data = std::make_shared<Data>();
    data->notes = std::move(notes);
    data->modifiers = std::move(modifiers);
    data->modifier_seed = modifier_seed;
    data->lanes = std::move(lanes);
    return runtime::Result<MidiContent, ModelError>(runtime::Ok(MidiContent(std::move(data))));
}

const MidiExpressionLane* MidiContent::lane_for(const MidiLaneAddress& address) const noexcept {
    const auto& lanes = data_->lanes;
    const auto found = std::lower_bound(lanes.begin(), lanes.end(), address,
                                        [](const MidiExpressionLane& lane,
                                           const MidiLaneAddress& wanted) {
                                            return lane.address < wanted;
                                        });
    return found != lanes.end() && found->address == address ? &*found : nullptr;
}

const NoteModifier* MidiContent::modifier_for(ItemId note_id) const noexcept {
    const auto& modifiers = data_->modifiers;
    const auto found = std::lower_bound(modifiers.begin(), modifiers.end(), note_id.value,
                                        [](const NoteModifier& entry, std::uint64_t wanted) {
                                            return entry.note_id.value < wanted;
                                        });
    return found != modifiers.end() && found->note_id == note_id ? &*found : nullptr;
}

runtime::Result<MidiContent, ModelError> MidiContent::replace_note(NoteEvent note) const {
    if (!note.id.valid() || note.duration.value <= 0 || note.pitch > 127 || note.channel > 15)
        return fail<MidiContent>(ModelErrorCode::InvalidNote, note.id);
    auto replacement = data_->notes;
    const auto found =
        std::find_if(replacement.begin(), replacement.end(),
                     [&](const NoteEvent& candidate) { return candidate.id == note.id; });
    if (found == replacement.end() || found->id != note.id)
        return fail<MidiContent>(ModelErrorCode::MissingItem, note.id);
    *found = note;
    return create(std::move(replacement), data_->modifiers, data_->modifier_seed, data_->lanes);
}

runtime::Result<OpaqueContent, ModelError>
OpaqueContent::create(SchemaIdentity schema, std::string raw_json, OpaqueContentLimits limits) {
    if (!schema.valid())
        return fail<OpaqueContent>(ModelErrorCode::InvalidSchemaIdentity);
    if (raw_json.size() > limits.max_input_bytes || raw_json.size() > limits.max_opaque_bytes)
        return fail<OpaqueContent>(ModelErrorCode::OpaqueContentLimitExceeded);
    DecodeLimits decode_limits;
    decode_limits.max_input_bytes = limits.max_input_bytes;
    decode_limits.max_depth = limits.max_depth;
    decode_limits.max_total_values = limits.max_total_values;
    decode_limits.max_array_elements = limits.max_array_elements;
    decode_limits.max_object_members = limits.max_object_members;
    decode_limits.max_string_bytes = limits.max_string_bytes;
    decode_limits.max_opaque_bytes = limits.max_opaque_bytes;
    auto parsed = parse_json(raw_json, decode_limits);
    if (!parsed) {
        const auto code = parsed.error().code == PersistenceErrorCode::LimitExceeded
                              ? ModelErrorCode::OpaqueContentLimitExceeded
                              : ModelErrorCode::InvalidOpaqueContent;
        return fail<OpaqueContent>(code);
    }
    auto envelope =
        validate_exact_envelope(parsed.value()->root(), schema.type_name, schema.version);
    if (!envelope)
        return fail<OpaqueContent>(ModelErrorCode::InvalidOpaqueContent);
    return runtime::Result<OpaqueContent, ModelError>(
        runtime::Ok(OpaqueContent(std::move(schema), std::move(raw_json), limits)));
}

struct Project::Data {
    ItemId id;
    std::string name;
    std::uint64_t next_item_id;
    ItemId root_sequence_id;
    std::vector<MediaAsset> assets;
    std::vector<Sequence> sequences;
    timebase::TempoMap tempo_map;
    timebase::MeterMap meter_map;
    std::optional<SessionStart> session_start;
    std::optional<TuningReference> tuning;
    detail::IdentityDirectory identities;
    std::uint64_t sequence_compile_structure_token = 0;
};

bool detail::ProjectStateAccess::identities_equivalent(const Project& lhs,
                                                       const Project& rhs) noexcept {
    return lhs.data_->identities.equivalent(rhs.data_->identities);
}

std::vector<detail::IdentityRecord>
detail::ProjectStateAccess::identity_entries(const Project& project) {
    return project.data_->identities.entries();
}

runtime::Result<Project, ModelError>
detail::ProjectStateAccess::restore_identities(Project project,
                                               std::vector<detail::IdentityRecord> entries) {
    const auto active_entries = project.data_->identities.entries();
    std::sort(entries.begin(), entries.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.item < rhs.item; });
    if (std::adjacent_find(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.item == rhs.item;
        }) != entries.end())
        return fail<Project>(ModelErrorCode::DuplicateItemId);

    std::size_t active_index = 0;
    const auto find_entry = [&](ItemId id) -> const detail::IdentityRecord* {
        const auto found = std::lower_bound(entries.begin(), entries.end(), id,
                                            [](const detail::IdentityRecord& candidate,
                                               ItemId wanted) { return candidate.item < wanted; });
        return found != entries.end() && found->item == id ? &*found : nullptr;
    };
    for (const auto& entry : entries) {
        const auto& location = entry.location;
        const auto valid_owner = [](ItemId id, std::uint64_t next) {
            return !id.valid() || id.value < next;
        };
        if (!entry.item.valid() || entry.item.value >= project.next_item_id() ||
            !valid_owner(location.parent_id, project.next_item_id()) ||
            !valid_owner(location.sequence_id, project.next_item_id()) ||
            !valid_owner(location.track_id, project.next_item_id()) ||
            !valid_owner(location.clip_id, project.next_item_id()))
            return fail<Project>(ModelErrorCode::InvalidSchemaIdentity, entry.item);
        const auto invalid = ItemId{};
        const auto valid_shape = [&] {
            // Parent is canonical and, except for lane/scene-owned children,
            // recomputable from the item's own coordinates. An AutomationPoint,
            // Take, MidiLanePoint, or Slot carries its automation lane, take
            // lane, expression lane, or scene only in parent_id; ownership below
            // validates it without circularly re-deriving it from
            // (sequence, track, clip).
            if (location.kind != ItemKind::AutomationPoint && location.kind != ItemKind::Take &&
                location.kind != ItemKind::MidiLanePoint && location.kind != ItemKind::Slot &&
                location.parent_id != immediate_parent_id(location.kind, project.id(),
                                                          location.sequence_id, location.track_id,
                                                          location.clip_id))
                return false;
            switch (location.kind) {
            case ItemKind::Project:
                return entry.item == project.id() && location.sequence_id == invalid &&
                       location.track_id == invalid && location.clip_id == invalid;
            case ItemKind::Asset:
                return location.sequence_id == invalid && location.track_id == invalid &&
                       location.clip_id == invalid;
            case ItemKind::Sequence:
                return location.sequence_id == entry.item && location.track_id == invalid &&
                       location.clip_id == invalid;
            case ItemKind::Track:
                return location.sequence_id.valid() && location.sequence_id != entry.item &&
                       location.track_id == entry.item && location.clip_id == invalid;
            case ItemKind::Marker:
            case ItemKind::Region:
            case ItemKind::Scene:
                return location.sequence_id.valid() && location.sequence_id != entry.item &&
                       location.track_id == invalid && location.clip_id == invalid;
            case ItemKind::Slot:
                return location.sequence_id.valid() && location.parent_id.valid() &&
                       location.track_id == invalid && location.clip_id == invalid &&
                       entry.item != location.sequence_id && entry.item != location.parent_id;
            case ItemKind::Clip:
                return location.sequence_id.valid() && location.track_id.valid() &&
                       location.sequence_id != location.track_id &&
                       location.sequence_id != entry.item && location.track_id != entry.item &&
                       location.clip_id == entry.item;
            case ItemKind::Note:
            case ItemKind::MidiLane:
                return location.sequence_id.valid() && location.track_id.valid() &&
                       location.clip_id.valid() && location.sequence_id != location.track_id &&
                       location.sequence_id != location.clip_id &&
                       location.track_id != location.clip_id &&
                       entry.item != location.sequence_id && entry.item != location.track_id &&
                       entry.item != location.clip_id;
            case ItemKind::MidiLanePoint:
                // parent_id is the owning expression lane (validated below); the
                // clip stays a coordinate because a lane never outlives its clip.
                return location.sequence_id.valid() && location.track_id.valid() &&
                       location.clip_id.valid() && location.parent_id.valid() &&
                       location.sequence_id != location.track_id &&
                       location.sequence_id != location.clip_id &&
                       location.track_id != location.clip_id &&
                       entry.item != location.sequence_id && entry.item != location.track_id &&
                       entry.item != location.clip_id && entry.item != location.parent_id;
            case ItemKind::DevicePlacement:
            case ItemKind::AutomationLane:
            case ItemKind::Modulator:
            case ItemKind::MacroControl:
            case ItemKind::ModulationRoute:
            case ItemKind::TakeLane:
                return location.sequence_id.valid() && location.track_id.valid() &&
                       location.sequence_id != location.track_id &&
                       entry.item != location.sequence_id && entry.item != location.track_id &&
                       location.clip_id == invalid;
            case ItemKind::AutomationPoint:
            case ItemKind::Take:
                // parent_id is the owning lane (validated by ownership below).
                return location.sequence_id.valid() && location.track_id.valid() &&
                       location.parent_id.valid() && location.clip_id == invalid &&
                       entry.item != location.sequence_id && entry.item != location.track_id &&
                       entry.item != location.parent_id;
            }
            return false;
        }();
        if (!valid_shape)
            return fail<Project>(ModelErrorCode::InvalidSchemaIdentity, entry.item);
        const auto owner_is = [&](ItemId id, ItemKind kind) {
            const auto* owner = find_entry(id);
            return owner && owner->location.kind == kind;
        };
        const auto valid_owners = [&] {
            switch (location.kind) {
            case ItemKind::Project:
                return true;
            case ItemKind::Asset:
            case ItemKind::Sequence:
                return owner_is(location.parent_id, ItemKind::Project);
            case ItemKind::Track:
            case ItemKind::Marker:
            case ItemKind::Region:
            case ItemKind::Scene:
                return owner_is(location.parent_id, ItemKind::Sequence);
            case ItemKind::Slot: {
                const auto* scene = find_entry(location.parent_id);
                return scene && scene->location.kind == ItemKind::Scene &&
                       scene->location.parent_id == location.sequence_id;
            }
            case ItemKind::Clip: {
                const auto* track = find_entry(location.parent_id);
                return track && track->location.kind == ItemKind::Track &&
                       track->location.parent_id == location.sequence_id;
            }
            case ItemKind::Note:
            case ItemKind::MidiLane: {
                const auto* clip = find_entry(location.parent_id);
                if (!clip || clip->location.kind != ItemKind::Clip ||
                    clip->location.parent_id != location.track_id ||
                    clip->location.sequence_id != location.sequence_id)
                    return false;
                const auto* track = find_entry(clip->location.parent_id);
                return track && track->location.kind == ItemKind::Track &&
                       track->location.parent_id == location.sequence_id;
            }
            case ItemKind::DevicePlacement:
            case ItemKind::AutomationLane:
            case ItemKind::Modulator:
            case ItemKind::MacroControl:
            case ItemKind::ModulationRoute:
            case ItemKind::TakeLane: {
                const auto* track = find_entry(location.parent_id);
                return track && track->location.kind == ItemKind::Track &&
                       track->location.parent_id == location.sequence_id;
            }
            case ItemKind::AutomationPoint: {
                const auto* lane = find_entry(location.parent_id);
                if (!lane || lane->location.kind != ItemKind::AutomationLane ||
                    lane->location.sequence_id != location.sequence_id ||
                    lane->location.track_id != location.track_id)
                    return false;
                const auto* track = find_entry(lane->location.parent_id);
                return track && track->location.kind == ItemKind::Track &&
                       track->location.parent_id == location.sequence_id;
            }
            case ItemKind::Take: {
                const auto* lane = find_entry(location.parent_id);
                if (!lane || lane->location.kind != ItemKind::TakeLane ||
                    lane->location.sequence_id != location.sequence_id ||
                    lane->location.track_id != location.track_id)
                    return false;
                const auto* track = find_entry(lane->location.parent_id);
                return track && track->location.kind == ItemKind::Track &&
                       track->location.parent_id == location.sequence_id;
            }
            case ItemKind::MidiLanePoint: {
                const auto* lane = find_entry(location.parent_id);
                return lane && lane->location.kind == ItemKind::MidiLane &&
                       lane->location.sequence_id == location.sequence_id &&
                       lane->location.track_id == location.track_id &&
                       lane->location.clip_id == location.clip_id;
            }
            }
            return false;
        }();
        if (!valid_owners)
            return fail<Project>(ModelErrorCode::InvalidSchemaIdentity, entry.item);
        if (location.active) {
            if (active_index >= active_entries.size() ||
                active_entries[active_index].item != entry.item ||
                active_entries[active_index].location != location)
                return fail<Project>(ModelErrorCode::InvalidSchemaIdentity, entry.item);
            ++active_index;
        } else if (project.data_->identities.locate(entry.item)) {
            return fail<Project>(ModelErrorCode::InvalidSchemaIdentity, entry.item);
        }
    }
    if (active_index != active_entries.size())
        return fail<Project>(ModelErrorCode::InvalidSchemaIdentity);
    if (entries.size() == active_entries.size())
        return runtime::Ok(std::move(project));
    auto restored = detail::IdentityDirectory::from_sorted_entries(entries);
    auto next_data = *project.data_;
    next_data.identities = std::move(restored);
    project.data_ = std::make_shared<const Project::Data>(std::move(next_data));
    return runtime::Ok(std::move(project));
}

runtime::Result<Project, ModelError> Project::create(ProjectInput input) {
    if (!input.id.valid())
        return fail<Project>(ModelErrorCode::InvalidItemId, input.id);

    for (auto& asset : input.assets) {
        if (const auto error = validate_media_asset(asset))
            return runtime::Result<Project, ModelError>(runtime::Err(*error));
    }
    std::size_t identity_count = 0;
    visit_project_identities(input, [&](ItemId, ItemLocation) { ++identity_count; });
    std::vector<detail::IdentityRecord> identity_entries;
    identity_entries.reserve(identity_count);
    // A session origin must be a real point on a real clock: a valid rate and a
    // non-negative offset. The rate is normalized so the same instant expressed
    // as 48000/1 and 96000/2 stores and compares identically.
    if (input.session_start) {
        if (!input.session_start->sample_rate.valid() || input.session_start->start.value < 0)
            return fail<Project>(ModelErrorCode::InvalidSessionStart, input.id);
        input.session_start->sample_rate = input.session_start->sample_rate.normalized();
    }
    if (input.tuning && !valid_tuning_reference(*input.tuning))
        return fail<Project>(ModelErrorCode::InvalidTuningReference, input.id);
    visit_project_identities(input, [&](ItemId id, ItemLocation item_location) {
        identity_entries.push_back({id, item_location});
    });
    std::sort(identity_entries.begin(), identity_entries.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.item < rhs.item; });
    if (const auto duplicate = std::adjacent_find(
            identity_entries.begin(), identity_entries.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.item == rhs.item; });
        duplicate != identity_entries.end())
        return fail<Project>(ModelErrorCode::DuplicateItemId, duplicate->item);
    const auto maximum_id = identity_entries.back().item.value;
    if (input.next_item_id == 0 || input.next_item_id <= maximum_id)
        return fail<Project>(ModelErrorCode::NextItemIdNotMonotonic, {input.next_item_id},
                             {maximum_id});

    std::sort(input.assets.begin(), input.assets.end(),
              [](const MediaAsset& lhs, const MediaAsset& rhs) { return lhs.id < rhs.id; });
    std::sort(input.sequences.begin(), input.sequences.end(),
              [](const Sequence& lhs, const Sequence& rhs) { return lhs.id() < rhs.id(); });
    const auto root =
        std::lower_bound(input.sequences.begin(), input.sequences.end(), input.root_sequence_id,
                         [](const Sequence& sequence, ItemId id) { return sequence.id() < id; });
    if (root == input.sequences.end() || root->id() != input.root_sequence_id)
        return fail<Project>(ModelErrorCode::MissingRootSequence, input.root_sequence_id);
    for (const auto& sequence : input.sequences)
        if (const auto error = detail::validate_sequence_media(input.assets, sequence))
            return fail<Project>(error->code, error->item, error->related_item);
    if (const auto error = validate_sequence_graph(input.sequences))
        return fail<Project>(error->code, error->item, error->related_item);
    auto identities = detail::IdentityDirectory::from_sorted_entries(identity_entries);
    return runtime::Result<Project, ModelError>(runtime::Ok(Project(std::make_shared<const Data>(
        Data{.id = input.id,
             .name = std::move(input.name),
             .next_item_id = input.next_item_id,
             .root_sequence_id = input.root_sequence_id,
             .assets = std::move(input.assets),
             .sequences = std::move(input.sequences),
             .tempo_map = std::move(input.tempo_map),
             .meter_map = std::move(input.meter_map),
             .session_start = input.session_start,
             .tuning = input.tuning,
             .identities = std::move(identities),
             .sequence_compile_structure_token = next_sequence_compile_structure_token()}))));
}

ItemId Project::id() const noexcept {
    return data_->id;
}
const std::string& Project::name() const noexcept {
    return data_->name;
}
std::uint64_t Project::next_item_id() const noexcept {
    return data_->next_item_id;
}
ItemId Project::root_sequence_id() const noexcept {
    return data_->root_sequence_id;
}
std::span<const MediaAsset> Project::assets() const noexcept {
    return data_->assets;
}
std::span<const Sequence> Project::sequences() const noexcept {
    return data_->sequences;
}
const timebase::TempoMap& Project::tempo_map() const noexcept {
    return data_->tempo_map;
}
const std::optional<SessionStart>& Project::session_start() const noexcept {
    return data_->session_start;
}
const std::optional<TuningReference>& Project::tuning() const noexcept {
    return data_->tuning;
}
const timebase::MeterMap& Project::meter_map() const noexcept {
    return data_->meter_map;
}
const MediaAsset* Project::find_asset(ItemId id) const noexcept {
    const auto found =
        std::lower_bound(data_->assets.begin(), data_->assets.end(), id,
                         [](const MediaAsset& asset, ItemId wanted) { return asset.id < wanted; });
    return found != data_->assets.end() && found->id == id ? &*found : nullptr;
}
const Sequence* Project::find_sequence(ItemId id) const noexcept {
    const auto found = std::lower_bound(
        data_->sequences.begin(), data_->sequences.end(), id,
        [](const Sequence& sequence, ItemId wanted) { return sequence.id() < wanted; });
    return found != data_->sequences.end() && found->id() == id ? &*found : nullptr;
}

std::optional<ItemLocation> Project::locate(ItemId id) const noexcept {
    return data_->identities.locate(id);
}

runtime::Result<Project, ModelError>
Project::replace_sequence(Sequence sequence, std::span<const IdentityMutation> mutations,
                          std::optional<std::uint64_t> requested_next) const {
    const auto found =
        std::lower_bound(data_->sequences.begin(), data_->sequences.end(), sequence.id(),
                         [](const Sequence& candidate, ItemId id) { return candidate.id() < id; });
    if (found == data_->sequences.end() || found->id() != sequence.id())
        return fail<Project>(ModelErrorCode::MissingItem, sequence.id());
    auto identities = data_->identities;
    if (const auto error = apply_identity_mutations(identities, mutations))
        return runtime::Result<Project, ModelError>(runtime::Err(*error));
    const auto next = requested_next.value_or(data_->next_item_id);
    if (next < data_->next_item_id || next == 0)
        return fail<Project>(ModelErrorCode::NextItemIdNotMonotonic, {next}, {data_->next_item_id});
    const auto index = static_cast<std::size_t>(found - data_->sequences.begin());
    const bool compile_shape_changed = !found->shares_compile_structure_with(sequence);
    const bool references_changed = !std::equal(
        found->outgoing_sequence_refs().begin(), found->outgoing_sequence_refs().end(),
        sequence.outgoing_sequence_refs().begin(), sequence.outgoing_sequence_refs().end());
    auto sequences = data_->sequences;
    sequences[index] = std::move(sequence);
    if (references_changed)
        if (const auto error = validate_sequence_graph(sequences))
            return runtime::Result<Project, ModelError>(runtime::Err(*error));
    auto next_data = *data_;
    next_data.next_item_id = next;
    next_data.sequences = std::move(sequences);
    next_data.identities = std::move(identities);
    if (compile_shape_changed)
        next_data.sequence_compile_structure_token = next_sequence_compile_structure_token();
    return runtime::Result<Project, ModelError>(
        runtime::Ok(Project(std::make_shared<const Data>(std::move(next_data)))));
}

runtime::Result<Project, ModelError>
Project::append_sequence(Sequence sequence, std::span<const IdentityMutation> mutations,
                         std::optional<std::uint64_t> requested_next) const {
    if (find_sequence(sequence.id()))
        return fail<Project>(ModelErrorCode::DuplicateItemId, sequence.id());
    auto sequences = data_->sequences;
    const auto position =
        std::lower_bound(sequences.begin(), sequences.end(), sequence.id(),
                         [](const Sequence& candidate, ItemId id) { return candidate.id() < id; });
    sequences.insert(position, std::move(sequence));
    if (const auto error = validate_sequence_graph(sequences))
        return runtime::Result<Project, ModelError>(runtime::Err(*error));
    auto identities = data_->identities;
    if (const auto error = apply_identity_mutations(identities, mutations))
        return runtime::Result<Project, ModelError>(runtime::Err(*error));
    const auto next = requested_next.value_or(data_->next_item_id);
    if (next < data_->next_item_id || next == 0)
        return fail<Project>(ModelErrorCode::NextItemIdNotMonotonic, {next}, {data_->next_item_id});
    auto next_data = *data_;
    next_data.next_item_id = next;
    next_data.sequences = std::move(sequences);
    next_data.identities = std::move(identities);
    next_data.sequence_compile_structure_token = next_sequence_compile_structure_token();
    return runtime::Ok(Project(std::make_shared<const Data>(std::move(next_data))));
}

runtime::Result<Project, ModelError>
Project::remove_sequence(ItemId sequence_id, std::span<const IdentityMutation> mutations) const {
    if (sequence_id == data_->root_sequence_id)
        return fail<Project>(ModelErrorCode::MissingRootSequence, sequence_id);
    const auto found =
        std::lower_bound(data_->sequences.begin(), data_->sequences.end(), sequence_id,
                         [](const Sequence& candidate, ItemId id) { return candidate.id() < id; });
    if (found == data_->sequences.end() || found->id() != sequence_id)
        return fail<Project>(ModelErrorCode::MissingItem, sequence_id);
    for (const auto& candidate : data_->sequences)
        if (std::binary_search(candidate.outgoing_sequence_refs().begin(),
                               candidate.outgoing_sequence_refs().end(), sequence_id))
            return fail<Project>(ModelErrorCode::MissingSequenceReference, candidate.id(),
                                 sequence_id);
    auto identities = data_->identities;
    if (const auto error = apply_identity_mutations(identities, mutations))
        return runtime::Result<Project, ModelError>(runtime::Err(*error));
    auto sequences = data_->sequences;
    sequences.erase(sequences.begin() + (found - data_->sequences.begin()));
    auto next_data = *data_;
    next_data.sequences = std::move(sequences);
    next_data.identities = std::move(identities);
    next_data.sequence_compile_structure_token = next_sequence_compile_structure_token();
    return runtime::Ok(Project(std::make_shared<const Data>(std::move(next_data))));
}

runtime::Result<Project, ModelError>
Project::append_asset(MediaAsset asset, std::span<const IdentityMutation> mutations,
                      std::optional<std::uint64_t> requested_next) const {
    if (const auto error = validate_media_asset(asset))
        return runtime::Result<Project, ModelError>(runtime::Err(*error));
    // The asset table is a set keyed by identity; a live entry with this id is a
    // duplicate the caller must resolve before append (a tombstoned id is
    // reactivated through the identity mutation below).
    if (find_asset(asset.id) != nullptr)
        return fail<Project>(ModelErrorCode::DuplicateItemId, asset.id);
    auto identities = data_->identities;
    if (const auto error = apply_identity_mutations(identities, mutations))
        return runtime::Result<Project, ModelError>(runtime::Err(*error));
    const auto next = requested_next.value_or(data_->next_item_id);
    if (next < data_->next_item_id || next == 0)
        return fail<Project>(ModelErrorCode::NextItemIdNotMonotonic, {next}, {data_->next_item_id});
    auto assets = data_->assets;
    const auto position =
        std::lower_bound(assets.begin(), assets.end(), asset.id,
                         [](const MediaAsset& candidate, ItemId id) { return candidate.id < id; });
    assets.insert(position, std::move(asset));
    return runtime::Result<Project, ModelError>(runtime::Ok(Project(std::make_shared<const Data>(
        Data{.id = data_->id,
             .name = data_->name,
             .next_item_id = next,
             .root_sequence_id = data_->root_sequence_id,
             .assets = std::move(assets),
             .sequences = data_->sequences,
             .tempo_map = data_->tempo_map,
             .meter_map = data_->meter_map,
             .session_start = data_->session_start,
             .tuning = data_->tuning,
             .identities = std::move(identities),
             .sequence_compile_structure_token = data_->sequence_compile_structure_token}))));
}

runtime::Result<Project, ModelError>
Project::remove_asset(ItemId asset_id, std::span<const IdentityMutation> mutations) const {
    const auto found =
        std::lower_bound(data_->assets.begin(), data_->assets.end(), asset_id,
                         [](const MediaAsset& candidate, ItemId id) { return candidate.id < id; });
    if (found == data_->assets.end() || found->id != asset_id)
        return fail<Project>(ModelErrorCode::MissingAsset, asset_id);
    // Referential integrity: an asset that any clip or take still plays cannot
    // be removed, or replay would resurrect a MediaRef pointing at a missing asset.
    static_assert(kClipContentAlternativeCount == 6,
                  "ClipContent gained an alternative: this scan decides an asset is unused "
                  "by looking only at MediaRef. Content that references an asset any other "
                  "way would let the asset be removed out from under it. Teach this scan "
                  "how the new content reaches assets before widening the variant.");
    for (const auto& sequence : data_->sequences)
        for (const auto& track : sequence.tracks()) {
            for (const auto& clip : track.clips())
                if (const auto* media = std::get_if<MediaRef>(&clip.content());
                    media && media->asset_id == asset_id)
                    return fail<Project>(ModelErrorCode::MissingAsset, clip.id(), asset_id);
            for (const auto& lane : track.take_lanes())
                for (const auto& take : lane.takes())
                    if (take.media().asset_id == asset_id)
                        return fail<Project>(ModelErrorCode::MissingAsset, take.id(), asset_id);
            if (track.freeze() && track.freeze()->media.asset_id == asset_id)
                return fail<Project>(ModelErrorCode::MissingAsset, track.id(), asset_id);
        }
    auto identities = data_->identities;
    if (const auto error = apply_identity_mutations(identities, mutations))
        return runtime::Result<Project, ModelError>(runtime::Err(*error));
    auto assets = data_->assets;
    assets.erase(assets.begin() + (found - data_->assets.begin()));
    return runtime::Result<Project, ModelError>(runtime::Ok(Project(std::make_shared<const Data>(
        Data{.id = data_->id,
             .name = data_->name,
             .next_item_id = data_->next_item_id,
             .root_sequence_id = data_->root_sequence_id,
             .assets = std::move(assets),
             .sequences = data_->sequences,
             .tempo_map = data_->tempo_map,
             .meter_map = data_->meter_map,
             .session_start = data_->session_start,
             .tuning = data_->tuning,
             .identities = std::move(identities),
             .sequence_compile_structure_token = data_->sequence_compile_structure_token}))));
}

Project Project::replace_tempo_map(timebase::TempoMap tempo_map) const {
    auto next_data = *data_;
    next_data.tempo_map = std::move(tempo_map);
    return Project(std::make_shared<const Data>(std::move(next_data)));
}

Project Project::replace_meter_map(timebase::MeterMap meter_map) const {
    auto next_data = *data_;
    next_data.meter_map = std::move(meter_map);
    return Project(std::make_shared<const Data>(std::move(next_data)));
}

std::size_t Project::shared_identity_nodes_with(const Project& other) const {
    return data_->identities.shared_nodes_with(other.data_->identities);
}

bool Project::shares_storage_with(const Project& other) const noexcept {
    return data_.get() == other.data_.get();
}

SequenceCompileStructureToken Project::sequence_compile_structure_token() const noexcept {
    return SequenceCompileStructureToken(data_->sequence_compile_structure_token);
}

ProjectIdentityStats Project::identity_stats() noexcept {
    return detail::IdentityDirectory::stats();
}

} // namespace pulp::timeline
