#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_json.hpp>

#include "identity_directory.hpp"
#include "identity_transition.hpp"
#include "project_state_access.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pulp::timeline {

namespace {

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

// Validates a media asset and canonicalizes its representation order. Shared by
// project construction and asset-append so both paths enforce the identical
// sealed-identity invariant: an asset with an invalid or empty ContentHash is
// rejected and never enters the document. Returns nullopt on success.
std::optional<ModelError> validate_media_asset(MediaAsset& asset) {
    if (!asset.id.valid())
        return ModelError{ModelErrorCode::InvalidItemId, asset.id, {}};
    if (!asset.sample_rate.valid())
        return ModelError{ModelErrorCode::InvalidSampleRate, asset.id, {}};
    if (!asset.content_hash.valid())
        return ModelError{ModelErrorCode::InvalidContentHash, asset.id, {}};
    for (const auto& locator : asset.locators)
        if (locator.hint.empty())
            return ModelError{ModelErrorCode::InvalidAssetLocator, asset.id, {}};
    std::vector<std::string_view> roles;
    roles.reserve(asset.representations.size());
    for (const auto& representation : asset.representations) {
        if (!representation.content_hash.valid())
            return ModelError{ModelErrorCode::InvalidContentHash, asset.id, {}};
        if (representation.role.empty())
            return ModelError{ModelErrorCode::InvalidAssetLocator, asset.id, {}};
        roles.push_back(representation.role);
        for (const auto& locator : representation.locators)
            if (locator.hint.empty())
                return ModelError{ModelErrorCode::InvalidAssetLocator, asset.id, {}};
    }
    std::sort(roles.begin(), roles.end());
    if (std::adjacent_find(roles.begin(), roles.end()) != roles.end())
        return ModelError{ModelErrorCode::DuplicateAssetRepresentation, asset.id, {}};
    std::sort(asset.representations.begin(), asset.representations.end(),
              [](const AssetRepresentation& lhs, const AssetRepresentation& rhs) {
                  return lhs.role < rhs.role;
              });
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

runtime::Result<NoteContent, ModelError> NoteContent::create(std::vector<NoteEvent> notes) {
    for (const auto& note : notes) {
        if (!note.id.valid())
            return fail<NoteContent>(ModelErrorCode::InvalidItemId, note.id);
        if (!positive_range(note.start.value, note.duration.value) || note.pitch > 127 ||
            note.channel > 15)
            return fail<NoteContent>(ModelErrorCode::InvalidNote, note.id);
    }
    if (const auto duplicate =
            first_duplicate(notes, [](const NoteEvent& note) { return note.id; }))
        return fail<NoteContent>(ModelErrorCode::DuplicateItemId, *duplicate);
    std::sort(notes.begin(), notes.end(), [](const NoteEvent& lhs, const NoteEvent& rhs) {
        return std::pair(lhs.start.value, lhs.id.value) < std::pair(rhs.start.value, rhs.id.value);
    });
    return runtime::Result<NoteContent, ModelError>(
        runtime::Ok(NoteContent(std::make_shared<const std::vector<NoteEvent>>(std::move(notes)))));
}

runtime::Result<NoteContent, ModelError> NoteContent::replace_note(NoteEvent note) const {
    if (!note.id.valid() || note.duration.value <= 0 || note.pitch > 127 || note.channel > 15)
        return fail<NoteContent>(ModelErrorCode::InvalidNote, note.id);
    auto replacement = *notes_;
    const auto found =
        std::find_if(replacement.begin(), replacement.end(),
                     [&](const NoteEvent& candidate) { return candidate.id == note.id; });
    if (found == replacement.end() || found->id != note.id)
        return fail<NoteContent>(ModelErrorCode::MissingItem, note.id);
    *found = note;
    return create(std::move(replacement));
}

runtime::Result<Take, ModelError> Take::create(ItemId id, MediaRef media,
                                               timebase::SamplePosition placement_start,
                                               timebase::RationalRate sample_rate) {
    if (!id.valid())
        return fail<Take>(ModelErrorCode::InvalidItemId, id);
    // The asset reference is external and its existence is validated where the
    // take is attached to a Project. Here the take only guarantees a well-formed
    // reference shape and a positive, playable region.
    if (!media.asset_id.valid())
        return fail<Take>(ModelErrorCode::InvalidTake, id, media.asset_id);
    if (media.frame_count == 0 || placement_start.value < 0 || !sample_rate.valid())
        return fail<Take>(ModelErrorCode::InvalidTake, id);
    return runtime::Result<Take, ModelError>(
        runtime::Ok(Take(id, media, placement_start, sample_rate)));
}

struct TakeLane::Data {
    ItemId id;
    std::string name;
    std::vector<Take> takes;
};

runtime::Result<TakeLane, ModelError> TakeLane::create(ItemId id, std::string name,
                                                       std::vector<Take> takes) {
    if (!id.valid())
        return fail<TakeLane>(ModelErrorCode::InvalidItemId, id);
    for (const auto& take : takes)
        if (!take.id().valid())
            return fail<TakeLane>(ModelErrorCode::InvalidTake, take.id());
    if (const auto duplicate = first_duplicate(takes, [](const Take& take) { return take.id(); }))
        return fail<TakeLane>(ModelErrorCode::DuplicateTake, *duplicate);
    std::sort(takes.begin(), takes.end(),
              [](const Take& lhs, const Take& rhs) { return lhs.id() < rhs.id(); });
    return runtime::Result<TakeLane, ModelError>(runtime::Ok(TakeLane(std::make_shared<const Data>(
        Data{.id = id, .name = std::move(name), .takes = std::move(takes)}))));
}

ItemId TakeLane::id() const noexcept {
    return data_->id;
}

const std::string& TakeLane::name() const noexcept {
    return data_->name;
}

std::span<const Take> TakeLane::takes() const noexcept {
    return data_->takes;
}

const Take* TakeLane::find_take(ItemId id) const noexcept {
    const auto found =
        std::lower_bound(data_->takes.begin(), data_->takes.end(), id,
                         [](const Take& take, ItemId wanted) { return take.id() < wanted; });
    return found != data_->takes.end() && found->id() == id ? &*found : nullptr;
}

runtime::Result<TakeLane, ModelError> TakeLane::insert_take(Take take) const {
    auto takes = data_->takes;
    const auto found = std::lower_bound(
        takes.begin(), takes.end(), take.id(),
        [](const Take& candidate, ItemId wanted) { return candidate.id() < wanted; });
    if (found != takes.end() && found->id() == take.id())
        return fail<TakeLane>(ModelErrorCode::DuplicateTake, take.id());
    takes.insert(found, std::move(take));
    return runtime::Ok(TakeLane(std::make_shared<const Data>(
        Data{.id = data_->id, .name = data_->name, .takes = std::move(takes)})));
}

runtime::Result<TakeLane, ModelError> TakeLane::erase_take(ItemId id) const {
    auto takes = data_->takes;
    const auto found =
        std::lower_bound(takes.begin(), takes.end(), id, [](const Take& candidate, ItemId wanted) {
            return candidate.id() < wanted;
        });
    if (found == takes.end() || found->id() != id)
        return fail<TakeLane>(ModelErrorCode::MissingItem, id, data_->id);
    takes.erase(found);
    return runtime::Ok(TakeLane(std::make_shared<const Data>(
        Data{.id = data_->id, .name = data_->name, .takes = std::move(takes)})));
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

struct Clip::Data {
    ItemId id;
    ClipTimeRange range;
    ClipContent content;
    ClipPlaybackProperties playback;
};

bool valid_playback_properties(ClipPlaybackProperties playback, std::uint64_t duration) noexcept {
    if (!std::isfinite(playback.gain_linear) || playback.gain_linear < 0.0f)
        return false;
    return playback.fade_in_duration <= duration && playback.fade_out_duration <= duration;
}

runtime::Result<Clip, ModelError> Clip::create(ItemId id, timebase::TickPosition start,
                                               timebase::TickDuration duration, ClipContent content,
                                               ClipPlaybackProperties playback) {
    if (!id.valid())
        return fail<Clip>(ModelErrorCode::InvalidItemId, id);
    if (!positive_range(start.value, duration.value))
        return fail<Clip>(ModelErrorCode::InvalidDuration, id);
    if (!valid_playback_properties(playback, static_cast<std::uint64_t>(duration.value)))
        return fail<Clip>(ModelErrorCode::InvalidClipPlaybackProperties, id);
    if (const auto* media = std::get_if<MediaRef>(&content)) {
        if (!media->asset_id.valid() || media->source_start.value < 0 || media->frame_count == 0 ||
            static_cast<std::uint64_t>(media->source_start.value) >
                std::numeric_limits<std::uint64_t>::max() - media->frame_count)
            return fail<Clip>(ModelErrorCode::InvalidMediaRange, id, media->asset_id);
    }
    return runtime::Result<Clip, ModelError>(runtime::Ok(Clip(std::make_shared<const Data>(
        Data{id, MusicalTimeRange{start, duration}, std::move(content), playback}))));
}

runtime::Result<Clip, ModelError> Clip::create_absolute(ItemId id, timebase::SamplePosition start,
                                                        std::uint64_t sample_count,
                                                        timebase::RationalRate sample_rate,
                                                        ClipContent content,
                                                        ClipPlaybackProperties playback) {
    if (!id.valid())
        return fail<Clip>(ModelErrorCode::InvalidItemId, id);
    if (!sample_rate.valid())
        return fail<Clip>(ModelErrorCode::InvalidSampleRate, id);
    if (sample_count == 0 ||
        sample_count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        start.value >
            std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(sample_count))
        return fail<Clip>(ModelErrorCode::InvalidDuration, id);
    if (!valid_playback_properties(playback, sample_count))
        return fail<Clip>(ModelErrorCode::InvalidClipPlaybackProperties, id);
    if (const auto* media = std::get_if<MediaRef>(&content)) {
        if (!media->asset_id.valid() || media->source_start.value < 0 || media->frame_count == 0 ||
            static_cast<std::uint64_t>(media->source_start.value) >
                std::numeric_limits<std::uint64_t>::max() - media->frame_count)
            return fail<Clip>(ModelErrorCode::InvalidMediaRange, id, media->asset_id);
    }
    return runtime::Result<Clip, ModelError>(runtime::Ok(Clip(std::make_shared<const Data>(
        Data{id, AbsoluteTimeRange{start, sample_count, sample_rate.normalized()},
             std::move(content), playback}))));
}

ItemId Clip::id() const noexcept {
    return data_->id;
}
ClipTimeAnchor Clip::time_anchor() const noexcept {
    return std::holds_alternative<MusicalTimeRange>(data_->range) ? ClipTimeAnchor::Musical
                                                                  : ClipTimeAnchor::Absolute;
}
const ClipTimeRange& Clip::time_range() const noexcept {
    return data_->range;
}
timebase::TickPosition Clip::start() const noexcept {
    const auto* range = std::get_if<MusicalTimeRange>(&data_->range);
    return range ? range->start : timebase::TickPosition{};
}
timebase::TickDuration Clip::duration() const noexcept {
    const auto* range = std::get_if<MusicalTimeRange>(&data_->range);
    return range ? range->duration : timebase::TickDuration{};
}
timebase::TickPosition Clip::end() const noexcept {
    return start() + duration();
}
timebase::SamplePosition Clip::absolute_start() const noexcept {
    const auto* range = std::get_if<AbsoluteTimeRange>(&data_->range);
    return range ? range->start : timebase::SamplePosition{};
}
std::uint64_t Clip::absolute_duration_samples() const noexcept {
    const auto* range = std::get_if<AbsoluteTimeRange>(&data_->range);
    return range ? range->sample_count : 0;
}
timebase::RationalRate Clip::absolute_sample_rate() const noexcept {
    const auto* range = std::get_if<AbsoluteTimeRange>(&data_->range);
    return range ? range->sample_rate : timebase::RationalRate{0, 1};
}
timebase::SamplePosition Clip::absolute_end() const noexcept {
    return {absolute_start().value + static_cast<std::int64_t>(absolute_duration_samples())};
}
const ClipContent& Clip::content() const noexcept {
    return data_->content;
}
ClipPlaybackProperties Clip::playback_properties() const noexcept {
    return data_->playback;
}

runtime::Result<Clip, ModelError> Clip::with_time_range(ClipTimeRange range) const {
    if (const auto* musical = std::get_if<MusicalTimeRange>(&range))
        return create(id(), musical->start, musical->duration, content(), playback_properties());
    const auto& absolute = std::get<AbsoluteTimeRange>(range);
    return create_absolute(id(), absolute.start, absolute.sample_count, absolute.sample_rate,
                           content(), playback_properties());
}

runtime::Result<Clip, ModelError> Clip::with_content(ClipContent replacement) const {
    if (time_anchor() == ClipTimeAnchor::Musical)
        return create(id(), start(), duration(), std::move(replacement), playback_properties());
    return create_absolute(id(), absolute_start(), absolute_duration_samples(),
                           absolute_sample_rate(), std::move(replacement), playback_properties());
}

runtime::Result<Clip, ModelError>
Clip::with_playback_properties(ClipPlaybackProperties playback) const {
    if (time_anchor() == ClipTimeAnchor::Musical)
        return create(id(), start(), duration(), content(), playback);
    return create_absolute(id(), absolute_start(), absolute_duration_samples(),
                           absolute_sample_rate(), content(), playback);
}

struct Sequence::Data {
    ItemId id;
    std::string name;
    std::optional<timebase::TickDuration> musical_duration;
    std::optional<AbsoluteTimelineDuration> absolute_duration;
    std::vector<Track> tracks;
    std::vector<std::pair<ItemId, std::size_t>> track_id_index;
};

runtime::Result<Sequence, ModelError>
Sequence::create(ItemId id, std::string name, std::optional<timebase::TickDuration> duration,
                 std::vector<Track> tracks) {
    return create(id, std::move(name), duration, std::nullopt, std::move(tracks));
}

runtime::Result<Sequence, ModelError> Sequence::create(
    ItemId id, std::string name, std::optional<timebase::TickDuration> musical_duration,
    std::optional<AbsoluteTimelineDuration> absolute_duration, std::vector<Track> tracks) {
    if (!id.valid())
        return fail<Sequence>(ModelErrorCode::InvalidItemId, id);
    if ((musical_duration && musical_duration->value < 0) ||
        (absolute_duration && !absolute_duration->sample_rate.valid()))
        return fail<Sequence>(ModelErrorCode::InvalidDuration, id);
    if (absolute_duration)
        absolute_duration->sample_rate = absolute_duration->sample_rate.normalized();
    std::vector<std::pair<ItemId, std::size_t>> by_id;
    by_id.reserve(tracks.size());
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        by_id.emplace_back(tracks[index].id(), index);
        for (const auto& clip : tracks[index].clips()) {
            if (clip.time_anchor() == ClipTimeAnchor::Musical && musical_duration &&
                (clip.start().value < 0 || clip.end().value > musical_duration->value))
                return fail<Sequence>(ModelErrorCode::InvalidDuration, clip.id(), id);
            if (clip.time_anchor() == ClipTimeAnchor::Absolute && absolute_duration &&
                (clip.absolute_sample_rate() != absolute_duration->sample_rate ||
                 clip.absolute_start().value < 0 ||
                 static_cast<std::uint64_t>(clip.absolute_end().value) >
                     absolute_duration->sample_count))
                return fail<Sequence>(ModelErrorCode::InvalidDuration, clip.id(), id);
        }
    }
    std::sort(by_id.begin(), by_id.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    const auto duplicate =
        std::adjacent_find(by_id.begin(), by_id.end(),
                           [](const auto& lhs, const auto& rhs) { return lhs.first == rhs.first; });
    if (duplicate != by_id.end())
        return fail<Sequence>(ModelErrorCode::DuplicateItemId, duplicate->first);
    return runtime::Result<Sequence, ModelError>(runtime::Ok(Sequence(
        std::make_shared<const Data>(Data{id, std::move(name), musical_duration, absolute_duration,
                                          std::move(tracks), std::move(by_id)}))));
}

ItemId Sequence::id() const noexcept {
    return data_->id;
}
const std::string& Sequence::name() const noexcept {
    return data_->name;
}
std::optional<timebase::TickDuration> Sequence::duration() const noexcept {
    return data_->musical_duration;
}
std::optional<AbsoluteTimelineDuration> Sequence::absolute_duration() const noexcept {
    return data_->absolute_duration;
}
std::span<const Track> Sequence::tracks() const noexcept {
    return data_->tracks;
}
const Track* Sequence::find_track(ItemId id) const noexcept {
    const auto found =
        std::lower_bound(data_->track_id_index.begin(), data_->track_id_index.end(), id,
                         [](const auto& entry, ItemId wanted) { return entry.first < wanted; });
    return found != data_->track_id_index.end() && found->first == id
               ? &data_->tracks[found->second]
               : nullptr;
}

runtime::Result<Sequence, ModelError> Sequence::replace_track(Track track) const {
    const auto found =
        std::lower_bound(data_->track_id_index.begin(), data_->track_id_index.end(), track.id(),
                         [](const auto& entry, ItemId wanted) { return entry.first < wanted; });
    if (found == data_->track_id_index.end() || found->first != track.id())
        return fail<Sequence>(ModelErrorCode::MissingItem, track.id(), data_->id);
    for (const auto& clip : track.clips()) {
        if (clip.time_anchor() == ClipTimeAnchor::Musical && data_->musical_duration &&
            (clip.start().value < 0 || clip.end().value > data_->musical_duration->value))
            return fail<Sequence>(ModelErrorCode::InvalidDuration, clip.id(), data_->id);
        if (clip.time_anchor() == ClipTimeAnchor::Absolute && data_->absolute_duration &&
            (clip.absolute_sample_rate() != data_->absolute_duration->sample_rate ||
             clip.absolute_start().value < 0 ||
             static_cast<std::uint64_t>(clip.absolute_end().value) >
                 data_->absolute_duration->sample_count))
            return fail<Sequence>(ModelErrorCode::InvalidDuration, clip.id(), data_->id);
    }
    auto tracks = data_->tracks;
    tracks[found->second] = std::move(track);
    return runtime::Result<Sequence, ModelError>(runtime::Ok(Sequence(std::make_shared<const Data>(
        Data{data_->id, data_->name, data_->musical_duration, data_->absolute_duration,
             std::move(tracks), data_->track_id_index}))));
}

bool Sequence::shares_storage_with(const Sequence& other) const noexcept {
    return data_.get() == other.data_.get();
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
    detail::IdentityDirectory identities;
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

    detail::IdentityDirectory restored;
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
            // Parent is canonical and, for every kind but AutomationPoint and
            // Take, recomputable from the item's own coordinates. An
            // AutomationPoint's parent is its automation lane and a Take's parent
            // is its take lane — neither lane is among (sequence, track, clip); it
            // is carried only in parent_id and is validated by ownership below,
            // never re-derived from coordinates here (that would be circular).
            if (location.kind != ItemKind::AutomationPoint && location.kind != ItemKind::Take &&
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
            case ItemKind::Clip:
                return location.sequence_id.valid() && location.track_id.valid() &&
                       location.sequence_id != location.track_id &&
                       location.sequence_id != entry.item && location.track_id != entry.item &&
                       location.clip_id == entry.item;
            case ItemKind::Note:
                return location.sequence_id.valid() && location.track_id.valid() &&
                       location.clip_id.valid() && location.sequence_id != location.track_id &&
                       location.sequence_id != location.clip_id &&
                       location.track_id != location.clip_id &&
                       entry.item != location.sequence_id && entry.item != location.track_id &&
                       entry.item != location.clip_id;
            case ItemKind::DevicePlacement:
            case ItemKind::AutomationLane:
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
                return owner_is(location.parent_id, ItemKind::Sequence);
            case ItemKind::Clip: {
                const auto* track = find_entry(location.parent_id);
                return track && track->location.kind == ItemKind::Track &&
                       track->location.parent_id == location.sequence_id;
            }
            case ItemKind::Note: {
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
        restored.insert(entry.item, location);
    }
    if (active_index != active_entries.size())
        return fail<Project>(ModelErrorCode::InvalidSchemaIdentity);
    auto next_data = *project.data_;
    next_data.identities = std::move(restored);
    project.data_ = std::make_shared<const Project::Data>(std::move(next_data));
    return runtime::Ok(std::move(project));
}

runtime::Result<Project, ModelError> Project::create(ProjectInput input) {
    if (!input.id.valid())
        return fail<Project>(ModelErrorCode::InvalidItemId, input.id);
    std::vector<ItemId> all_ids{input.id};
    std::uint64_t maximum_id = input.id.value;
    for (auto& asset : input.assets) {
        if (const auto error = validate_media_asset(asset))
            return runtime::Result<Project, ModelError>(runtime::Err(*error));
        all_ids.push_back(asset.id);
        maximum_id = std::max(maximum_id, asset.id.value);
    }
    for (const auto& sequence : input.sequences) {
        all_ids.push_back(sequence.id());
        maximum_id = std::max(maximum_id, sequence.id().value);
        for (const auto& track : sequence.tracks()) {
            all_ids.push_back(track.id());
            maximum_id = std::max(maximum_id, track.id().value);
            for (const auto& device : track.device_chain()) {
                all_ids.push_back(device.id);
                maximum_id = std::max(maximum_id, device.id.value);
            }
            for (const auto& lane : track.automation_lanes()) {
                all_ids.push_back(lane.id());
                maximum_id = std::max(maximum_id, lane.id().value);
                for (const auto& point : lane.curve().points()) {
                    all_ids.push_back(point.id);
                    maximum_id = std::max(maximum_id, point.id.value);
                }
            }
            for (const auto& take_lane : track.take_lanes()) {
                all_ids.push_back(take_lane.id());
                maximum_id = std::max(maximum_id, take_lane.id().value);
                for (const auto& take : take_lane.takes()) {
                    all_ids.push_back(take.id());
                    maximum_id = std::max(maximum_id, take.id().value);
                }
            }
            for (const auto& clip : track.clips()) {
                all_ids.push_back(clip.id());
                maximum_id = std::max(maximum_id, clip.id().value);
                if (const auto* notes = std::get_if<NoteContent>(&clip.content())) {
                    for (const auto& note : notes->notes()) {
                        all_ids.push_back(note.id);
                        maximum_id = std::max(maximum_id, note.id.value);
                    }
                }
            }
        }
    }
    std::sort(all_ids.begin(), all_ids.end());
    if (const auto duplicate = std::adjacent_find(all_ids.begin(), all_ids.end());
        duplicate != all_ids.end())
        return fail<Project>(ModelErrorCode::DuplicateItemId, *duplicate);
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
    const auto validate_media_ref = [&](const MediaRef& media,
                                        ItemId owner) -> std::optional<ModelError> {
        const auto found =
            std::lower_bound(input.assets.begin(), input.assets.end(), media.asset_id,
                             [](const MediaAsset& asset, ItemId id) { return asset.id < id; });
        if (found == input.assets.end() || found->id != media.asset_id)
            return ModelError{ModelErrorCode::MissingAsset, owner, media.asset_id};
        const auto source_start = static_cast<std::uint64_t>(media.source_start.value);
        if (source_start > found->frame_count ||
            media.frame_count > found->frame_count - source_start)
            return ModelError{ModelErrorCode::InvalidMediaRange, owner, media.asset_id};
        return std::nullopt;
    };
    for (const auto& sequence : input.sequences) {
        for (const auto& track : sequence.tracks()) {
            for (const auto& clip : track.clips()) {
                if (const auto* media = std::get_if<MediaRef>(&clip.content()))
                    if (const auto error = validate_media_ref(*media, clip.id()))
                        return fail<Project>(error->code, error->item, error->related_item);
            }
            // A take referencing a non-existent (or out-of-range) asset is
            // rejected fail-closed, exactly as a clip MediaRef is.
            for (const auto& take_lane : track.take_lanes())
                for (const auto& take : take_lane.takes())
                    if (const auto error = validate_media_ref(take.media(), take.id()))
                        return fail<Project>(error->code, error->item, error->related_item);
        }
    }
    detail::IdentityDirectory identities;
    auto add_identity = [&](ItemId id, ItemLocation location) { identities.insert(id, location); };
    const auto location = [&](ItemKind kind, ItemId sequence = {}, ItemId track = {},
                              ItemId clip = {}, ItemId lane = {}) {
        return ItemLocation{
            kind,     immediate_parent_id(kind, input.id, sequence, track, clip, lane),
            sequence, track,
            clip,     true};
    };
    add_identity(input.id, location(ItemKind::Project));
    for (const auto& asset : input.assets)
        add_identity(asset.id, location(ItemKind::Asset));
    for (const auto& sequence : input.sequences) {
        add_identity(sequence.id(), location(ItemKind::Sequence, sequence.id()));
        for (const auto& track : sequence.tracks()) {
            add_identity(track.id(), location(ItemKind::Track, sequence.id(), track.id()));
            for (const auto& device : track.device_chain())
                add_identity(device.id,
                             location(ItemKind::DevicePlacement, sequence.id(), track.id()));
            for (const auto& lane : track.automation_lanes()) {
                add_identity(lane.id(),
                             location(ItemKind::AutomationLane, sequence.id(), track.id()));
                for (const auto& point : lane.curve().points())
                    add_identity(point.id, location(ItemKind::AutomationPoint, sequence.id(),
                                                    track.id(), {}, lane.id()));
            }
            for (const auto& take_lane : track.take_lanes()) {
                add_identity(take_lane.id(),
                             location(ItemKind::TakeLane, sequence.id(), track.id()));
                for (const auto& take : take_lane.takes())
                    add_identity(take.id(), location(ItemKind::Take, sequence.id(), track.id(), {},
                                                     take_lane.id()));
            }
            for (const auto& clip : track.clips()) {
                add_identity(clip.id(),
                             location(ItemKind::Clip, sequence.id(), track.id(), clip.id()));
                if (const auto* notes = std::get_if<NoteContent>(&clip.content())) {
                    for (const auto& note : notes->notes())
                        add_identity(note.id, location(ItemKind::Note, sequence.id(), track.id(),
                                                       clip.id()));
                }
            }
        }
    }
    return runtime::Result<Project, ModelError>(runtime::Ok(
        Project(std::make_shared<const Data>(Data{.id = input.id,
                                                  .name = std::move(input.name),
                                                  .next_item_id = input.next_item_id,
                                                  .root_sequence_id = input.root_sequence_id,
                                                  .assets = std::move(input.assets),
                                                  .sequences = std::move(input.sequences),
                                                  .tempo_map = std::move(input.tempo_map),
                                                  .meter_map = std::move(input.meter_map),
                                                  .identities = std::move(identities)}))));
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
    auto sequences = data_->sequences;
    sequences[static_cast<std::size_t>(found - data_->sequences.begin())] = std::move(sequence);
    auto next_data = *data_;
    next_data.next_item_id = next;
    next_data.sequences = std::move(sequences);
    next_data.identities = std::move(identities);
    return runtime::Result<Project, ModelError>(
        runtime::Ok(Project(std::make_shared<const Data>(std::move(next_data)))));
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
        Data{data_->id, data_->name, next, data_->root_sequence_id, std::move(assets),
             data_->sequences, data_->tempo_map, data_->meter_map, std::move(identities)}))));
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
        }
    auto identities = data_->identities;
    if (const auto error = apply_identity_mutations(identities, mutations))
        return runtime::Result<Project, ModelError>(runtime::Err(*error));
    auto assets = data_->assets;
    assets.erase(assets.begin() + (found - data_->assets.begin()));
    return runtime::Result<Project, ModelError>(
        runtime::Ok(Project(std::make_shared<const Data>(Data{
            data_->id, data_->name, data_->next_item_id, data_->root_sequence_id, std::move(assets),
            data_->sequences, data_->tempo_map, data_->meter_map, std::move(identities)}))));
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

ProjectIdentityStats Project::identity_stats() noexcept {
    return detail::IdentityDirectory::stats();
}

} // namespace pulp::timeline
