#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_json.hpp>

#include "asset_validation.hpp"
#include "identity_directory.hpp"
#include "identity_transition.hpp"
#include "project_state_access.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

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
        if (!valid_locator_kind(locator.kind) || locator.hint.empty())
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
            if (!valid_locator_kind(locator.kind) || locator.hint.empty())
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
    if (asset.loop_info &&
        !detail::validate_and_canonicalize(*asset.loop_info, asset.frame_count))
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
    for (const auto& sequence : input.sequences) {
        visit(sequence.id(), location(ItemKind::Sequence, sequence.id()));
        for (const auto& marker : sequence.markers())
            visit(marker.id, location(ItemKind::Marker, sequence.id()));
        for (const auto& region : sequence.regions())
            visit(region.id, location(ItemKind::Region, sequence.id()));
        for (const auto& track : sequence.tracks()) {
            visit(track.id(), location(ItemKind::Track, sequence.id(), track.id()));
            for (const auto& device : track.device_chain())
                visit(device.id, location(ItemKind::DevicePlacement, sequence.id(), track.id()));
            for (const auto& lane : track.automation_lanes()) {
                visit(lane.id(), location(ItemKind::AutomationLane, sequence.id(), track.id()));
                for (const auto& point : lane.curve().points())
                    visit(point.id, location(ItemKind::AutomationPoint, sequence.id(), track.id(),
                                             {}, lane.id()));
            }
            for (const auto& take_lane : track.take_lanes()) {
                visit(take_lane.id(), location(ItemKind::TakeLane, sequence.id(), track.id()));
                for (const auto& take : take_lane.takes())
                    visit(take.id(),
                          location(ItemKind::Take, sequence.id(), track.id(), {}, take_lane.id()));
            }
            for (const auto& clip : track.clips()) {
                visit(clip.id(), location(ItemKind::Clip, sequence.id(), track.id(), clip.id()));
                if (const auto* notes = std::get_if<NoteContent>(&clip.content()))
                    for (const auto& note : notes->notes())
                        visit(note.id,
                              location(ItemKind::Note, sequence.id(), track.id(), clip.id()));
            }
        }
    }
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

namespace {

constexpr std::uint8_t kPitchClassCount = 12;

constexpr bool valid_chord_quality(ChordQuality quality) noexcept {
    switch (quality) {
    case ChordQuality::Major:
    case ChordQuality::Minor:
    case ChordQuality::Diminished:
    case ChordQuality::Augmented:
    case ChordQuality::Dominant7:
    case ChordQuality::Major7:
    case ChordQuality::Minor7:
    case ChordQuality::HalfDiminished7:
    case ChordQuality::Suspended2:
    case ChordQuality::Suspended4:
        return true;
    }
    return false;
}

constexpr bool valid_scale_mode(ScaleMode mode) noexcept {
    switch (mode) {
    case ScaleMode::Major:
    case ScaleMode::NaturalMinor:
    case ScaleMode::HarmonicMinor:
    case ScaleMode::MelodicMinor:
    case ScaleMode::Dorian:
    case ScaleMode::Phrygian:
    case ScaleMode::Lydian:
    case ScaleMode::Mixolydian:
    case ScaleMode::Locrian:
    case ScaleMode::Chromatic:
        return true;
    }
    return false;
}

// Markers and regions share the sequence's annotation identity space, so one
// ItemId can name at most one of them. Ordering is canonical, never authored:
// callers hand over any order and the sequence stores the sorted result.
bool marker_less(const SequenceMarker& lhs, const SequenceMarker& rhs) noexcept {
    return std::pair(lhs.position.value, lhs.id.value) <
           std::pair(rhs.position.value, rhs.id.value);
}

bool region_less(const SequenceRegion& lhs, const SequenceRegion& rhs) noexcept {
    return std::tuple(lhs.position.value, lhs.duration.value, lhs.id.value) <
           std::tuple(rhs.position.value, rhs.duration.value, rhs.id.value);
}

void sort_markers(std::vector<SequenceMarker>& markers) {
    std::sort(markers.begin(), markers.end(), marker_less);
}

void sort_regions(std::vector<SequenceRegion>& regions) {
    std::sort(regions.begin(), regions.end(), region_less);
}

// A musical duration bounds the annotation domain; without one the sequence is
// absolute-anchored and only the lower bound applies.
bool within_musical_span(std::int64_t start, std::int64_t length,
                         const std::optional<timebase::TickDuration>& musical) noexcept {
    if (start < 0 || length < 0 || start > std::numeric_limits<std::int64_t>::max() - length)
        return false;
    return !musical || start + length <= musical->value;
}

std::optional<ModelError>
validate_markers(const std::vector<SequenceMarker>& markers,
                 const std::optional<timebase::TickDuration>& musical) noexcept {
    for (const auto& marker : markers) {
        if (!marker.id.valid())
            return ModelError{ModelErrorCode::InvalidItemId, marker.id, {}};
        if (!within_musical_span(marker.position.value, 0, musical))
            return ModelError{ModelErrorCode::InvalidMarker, marker.id, {}};
    }
    return std::nullopt;
}

std::optional<ModelError>
validate_regions(const std::vector<SequenceRegion>& regions,
                 const std::optional<timebase::TickDuration>& musical) noexcept {
    for (const auto& region : regions) {
        if (!region.id.valid())
            return ModelError{ModelErrorCode::InvalidItemId, region.id, {}};
        // A zero-length span is a marker, not a region; reject it rather than
        // silently admitting an annotation that cannot be selected.
        if (region.duration.value <= 0 ||
            !within_musical_span(region.position.value, region.duration.value, musical))
            return ModelError{ModelErrorCode::InvalidRegion, region.id, {}};
    }
    return std::nullopt;
}

std::optional<ItemId> duplicate_annotation_id(const std::vector<SequenceMarker>& markers,
                                              const std::vector<SequenceRegion>& regions) {
    std::vector<ItemId> ids;
    ids.reserve(markers.size() + regions.size());
    for (const auto& marker : markers)
        ids.push_back(marker.id);
    for (const auto& region : regions)
        ids.push_back(region.id);
    std::sort(ids.begin(), ids.end());
    const auto duplicate = std::adjacent_find(ids.begin(), ids.end());
    return duplicate == ids.end() ? std::nullopt : std::optional<ItemId>(*duplicate);
}

} // namespace

runtime::Result<ChordScaleLane, ModelError>
ChordScaleLane::create(std::vector<ChordScaleEvent> events) {
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (event.position.value < 0 || event.chord_root >= kPitchClassCount ||
            event.scale_root >= kPitchClassCount || !valid_chord_quality(event.chord_quality) ||
            !valid_scale_mode(event.scale_mode))
            return fail<ChordScaleLane>(ModelErrorCode::InvalidChordScaleEvent);
        // Authored order is the document's order. Sorting a caller's events
        // here would silently accept a lane whose harmony the caller did not
        // mean, so an out-of-order or duplicated position is a rejection.
        if (index != 0 && events[index - 1].position.value >= event.position.value)
            return fail<ChordScaleLane>(ModelErrorCode::UnorderedChordScaleLane);
    }
    return runtime::Result<ChordScaleLane, ModelError>(runtime::Ok(ChordScaleLane(
        std::make_shared<const std::vector<ChordScaleEvent>>(std::move(events)))));
}

const ChordScaleEvent* ChordScaleLane::at(timebase::TickPosition position) const noexcept {
    const auto found = std::upper_bound(
        events_->begin(), events_->end(), position,
        [](timebase::TickPosition wanted, const ChordScaleEvent& event) {
            return wanted.value < event.position.value;
        });
    return found == events_->begin() ? nullptr : &*(found - 1);
}

bool ChordScaleLane::operator==(const ChordScaleLane& other) const noexcept {
    return events_.get() == other.events_.get() || *events_ == *other.events_;
}

namespace {

// Scale `value` by `strength` per-mille, rounding halves away from zero so a
// positive and a negative offset of the same size are attenuated by the same
// amount. Truncation would bias every groove toward zero displacement.
//
// Both call sites pass a value the template already bounded at construction (an
// offset smaller than a step, or a velocity deviation smaller than the scale
// ceiling), so negating the magnitude and multiplying by the strength stay well
// inside the signed domain.
std::int64_t scaled_by_strength(std::int64_t value, std::int32_t strength) noexcept {
    const auto magnitude = value < 0 ? -value : value;
    const auto scaled = (magnitude * strength + kGrooveUnitScale / 2) / kGrooveUnitScale;
    return value < 0 ? -scaled : scaled;
}

// Which entry of a repeating table `position` falls in. The table repeats in
// both directions, so the index floors toward negative infinity and the modulus
// is corrected into range rather than inheriting the sign of the dividend.
std::size_t groove_slot(std::int64_t position, std::int64_t step, std::size_t size) noexcept {
    const auto count = static_cast<std::int64_t>(size);
    auto index = position / step;
    if (position % step != 0 && (position < 0) != (step < 0))
        --index;
    auto slot = index % count;
    if (slot < 0)
        slot += count;
    return static_cast<std::size_t>(slot);
}

} // namespace

runtime::Result<GrooveTemplate, ModelError> GrooveTemplate::create(GrooveTemplateInput input) {
    const auto swings = input.swing_grid.value != 0;
    if (swings && !timebase::valid_swing_grid(input.swing_grid))
        return fail<GrooveTemplate>(ModelErrorCode::InvalidGrooveTemplate);
    if (!timebase::valid_swing_ratio(input.swing))
        return fail<GrooveTemplate>(ModelErrorCode::InvalidGrooveTemplate);
    // A step width and a table imply each other: a width with no entries names
    // nothing, and entries with no width have no position to be read at.
    if ((input.step.value != 0) != !input.steps.empty())
        return fail<GrooveTemplate>(ModelErrorCode::InvalidGrooveTemplate);
    if (input.step.value < 0 || input.step.value > timebase::kMaxSwingGridTicks ||
        input.steps.size() > kMaxGrooveSteps)
        return fail<GrooveTemplate>(ModelErrorCode::InvalidGrooveTemplate);
    if (input.timing_strength < 0 || input.timing_strength > kGrooveUnitScale ||
        input.velocity_strength < 0 || input.velocity_strength > kGrooveUnitScale)
        return fail<GrooveTemplate>(ModelErrorCode::InvalidGrooveTemplate);
    for (const auto& step : input.steps) {
        // An offset of a whole step or more would move material past the step
        // beyond its neighbour, which is a different table written wrong rather
        // than an extreme feel.
        const auto offset = step.timing_offset.value;
        if (offset <= -input.step.value || offset >= input.step.value)
            return fail<GrooveTemplate>(ModelErrorCode::InvalidGrooveTemplate);
        if (step.velocity_scale < 0 || step.velocity_scale > kMaxGrooveVelocityScale)
            return fail<GrooveTemplate>(ModelErrorCode::InvalidGrooveTemplate);
    }
    return runtime::Result<GrooveTemplate, ModelError>(
        runtime::Ok(GrooveTemplate(std::make_shared<const Data>(
            Data{std::move(input.name), input.swing_grid, input.swing, input.step,
                 std::move(input.steps), input.timing_strength, input.velocity_strength}))));
}

bool GrooveTemplate::states_no_feel() const noexcept {
    return data_->swing_grid.value == 0 && data_->steps.empty();
}

bool GrooveTemplate::is_canonical_default() const noexcept {
    return data_->name.empty() && data_->swing_grid.value == 0 &&
           data_->swing == timebase::kStraightSwing && data_->step.value == 0 &&
           data_->steps.empty() && data_->timing_strength == kGrooveUnitScale &&
           data_->velocity_strength == kGrooveUnitScale;
}

const GrooveStep* GrooveTemplate::step_at(timebase::TickPosition position) const noexcept {
    if (data_->steps.empty() || data_->step.value <= 0)
        return nullptr;
    return &data_->steps[groove_slot(position.value, data_->step.value, data_->steps.size())];
}

timebase::TickPosition
GrooveTemplate::apply_timing(timebase::TickPosition position) const noexcept {
    auto placed = position;
    if (data_->swing_grid.value != 0)
        placed = timebase::swing_position(position, data_->swing_grid, data_->swing);
    // The table is indexed by the authored position, not the swung one, so a
    // change of swing setting never re-assigns material to a different step.
    const auto* step = step_at(position);
    if (!step)
        return placed;
    return placed +
           timebase::TickDuration{scaled_by_strength(step->timing_offset.value,
                                                     data_->timing_strength)};
}

std::int32_t GrooveTemplate::velocity_scale_at(timebase::TickPosition position) const noexcept {
    const auto* step = step_at(position);
    if (!step)
        return kGrooveUnitScale;
    const auto deviation = static_cast<std::int64_t>(step->velocity_scale) -
                           static_cast<std::int64_t>(kGrooveUnitScale);
    return static_cast<std::int32_t>(kGrooveUnitScale +
                                     scaled_by_strength(deviation, data_->velocity_strength));
}

bool GrooveTemplate::operator==(const GrooveTemplate& other) const noexcept {
    return data_.get() == other.data_.get() || *data_ == *other.data_;
}

struct Sequence::Data {
    ItemId id;
    std::string name;
    std::optional<timebase::TickDuration> musical_duration;
    std::optional<AbsoluteTimelineDuration> absolute_duration;
    std::vector<Track> tracks;
    std::vector<std::pair<ItemId, std::size_t>> track_id_index;
    std::vector<SequenceMarker> markers;
    std::vector<SequenceRegion> regions;
    ChordScaleLane chord_scale_lane;
    GrooveTemplate groove;
};

runtime::Result<Sequence, ModelError>
Sequence::create(ItemId id, std::string name, std::optional<timebase::TickDuration> duration,
                 std::vector<Track> tracks) {
    return create(id, std::move(name), duration, std::nullopt, std::move(tracks));
}

runtime::Result<Sequence, ModelError> Sequence::create(
    ItemId id, std::string name, std::optional<timebase::TickDuration> musical_duration,
    std::optional<AbsoluteTimelineDuration> absolute_duration, std::vector<Track> tracks) {
    return create(id, std::move(name), musical_duration, absolute_duration, std::move(tracks), {},
                  {});
}

runtime::Result<Sequence, ModelError> Sequence::create(
    ItemId id, std::string name, std::optional<timebase::TickDuration> musical_duration,
    std::optional<AbsoluteTimelineDuration> absolute_duration, std::vector<Track> tracks,
    std::vector<SequenceMarker> markers, std::vector<SequenceRegion> regions) {
    auto empty_lane = ChordScaleLane::create({});
    if (!empty_lane)
        return runtime::Result<Sequence, ModelError>(runtime::Err(empty_lane.error()));
    return create(id, std::move(name), musical_duration, absolute_duration, std::move(tracks),
                  std::move(markers), std::move(regions), std::move(empty_lane).value());
}

runtime::Result<Sequence, ModelError> Sequence::create(
    ItemId id, std::string name, std::optional<timebase::TickDuration> musical_duration,
    std::optional<AbsoluteTimelineDuration> absolute_duration, std::vector<Track> tracks,
    std::vector<SequenceMarker> markers, std::vector<SequenceRegion> regions,
    ChordScaleLane chord_scale_lane) {
    return create(SequenceInput{id, std::move(name), musical_duration, absolute_duration,
                                std::move(tracks), std::move(markers), std::move(regions),
                                std::move(chord_scale_lane), std::nullopt});
}

runtime::Result<Sequence, ModelError> Sequence::create(SequenceInput input) {
    const auto id = input.id;
    auto musical_duration = input.musical_duration;
    auto absolute_duration = input.absolute_duration;
    auto tracks = std::move(input.tracks);
    auto markers = std::move(input.markers);
    auto regions = std::move(input.regions);
    // Absent context members mean the defaults the sequence would otherwise
    // carry; both validate on construction, so neither can be default-built.
    if (!input.chord_scale_lane) {
        auto empty_lane = ChordScaleLane::create({});
        if (!empty_lane)
            return runtime::Result<Sequence, ModelError>(runtime::Err(empty_lane.error()));
        input.chord_scale_lane = std::move(empty_lane).value();
    }
    if (!input.groove) {
        auto straight = GrooveTemplate::create({});
        if (!straight)
            return runtime::Result<Sequence, ModelError>(runtime::Err(straight.error()));
        input.groove = std::move(straight).value();
    }
    if (!id.valid())
        return fail<Sequence>(ModelErrorCode::InvalidItemId, id);
    if ((musical_duration && musical_duration->value < 0) ||
        (absolute_duration && !absolute_duration->sample_rate.valid()))
        return fail<Sequence>(ModelErrorCode::InvalidDuration, id);
    if (absolute_duration)
        absolute_duration->sample_rate = absolute_duration->sample_rate.normalized();
    if (const auto invalid = validate_markers(markers, musical_duration))
        return fail<Sequence>(invalid->code, invalid->item, id);
    if (const auto invalid = validate_regions(regions, musical_duration))
        return fail<Sequence>(invalid->code, invalid->item, id);
    if (const auto duplicate = duplicate_annotation_id(markers, regions))
        return fail<Sequence>(ModelErrorCode::DuplicateItemId, *duplicate, id);
    sort_markers(markers);
    sort_regions(regions);
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
    return runtime::Result<Sequence, ModelError>(runtime::Ok(Sequence(std::make_shared<const Data>(
        Data{id, std::move(input.name), musical_duration, absolute_duration, std::move(tracks),
             std::move(by_id), std::move(markers), std::move(regions),
             std::move(*input.chord_scale_lane), std::move(*input.groove)}))));
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
std::span<const SequenceMarker> Sequence::markers() const noexcept {
    return data_->markers;
}
std::span<const SequenceRegion> Sequence::regions() const noexcept {
    return data_->regions;
}
const SequenceMarker* Sequence::find_marker(ItemId id) const noexcept {
    const auto found = std::find_if(data_->markers.begin(), data_->markers.end(),
                                    [id](const SequenceMarker& marker) { return marker.id == id; });
    return found == data_->markers.end() ? nullptr : &*found;
}
const SequenceRegion* Sequence::find_region(ItemId id) const noexcept {
    const auto found = std::find_if(data_->regions.begin(), data_->regions.end(),
                                    [id](const SequenceRegion& region) { return region.id == id; });
    return found == data_->regions.end() ? nullptr : &*found;
}

runtime::Result<Sequence, ModelError>
Sequence::with_annotations(std::vector<SequenceMarker> markers,
                           std::vector<SequenceRegion> regions) const {
    if (const auto invalid = validate_markers(markers, data_->musical_duration))
        return fail<Sequence>(invalid->code, invalid->item, data_->id);
    if (const auto invalid = validate_regions(regions, data_->musical_duration))
        return fail<Sequence>(invalid->code, invalid->item, data_->id);
    if (const auto duplicate = duplicate_annotation_id(markers, regions))
        return fail<Sequence>(ModelErrorCode::DuplicateItemId, *duplicate, data_->id);
    sort_markers(markers);
    sort_regions(regions);
    return runtime::Result<Sequence, ModelError>(runtime::Ok(Sequence(std::make_shared<const Data>(
        Data{data_->id, data_->name, data_->musical_duration, data_->absolute_duration,
             data_->tracks, data_->track_id_index, std::move(markers), std::move(regions),
             data_->chord_scale_lane, data_->groove}))));
}

runtime::Result<Sequence, ModelError> Sequence::insert_marker(SequenceMarker marker) const {
    auto markers = data_->markers;
    markers.push_back(std::move(marker));
    return with_annotations(std::move(markers), data_->regions);
}

runtime::Result<Sequence, ModelError> Sequence::erase_marker(ItemId id) const {
    auto markers = data_->markers;
    const auto found = std::find_if(markers.begin(), markers.end(),
                                    [id](const SequenceMarker& marker) { return marker.id == id; });
    if (found == markers.end())
        return fail<Sequence>(ModelErrorCode::MissingItem, id, data_->id);
    markers.erase(found);
    return with_annotations(std::move(markers), data_->regions);
}

runtime::Result<Sequence, ModelError> Sequence::insert_region(SequenceRegion region) const {
    auto regions = data_->regions;
    regions.push_back(std::move(region));
    return with_annotations(data_->markers, std::move(regions));
}

runtime::Result<Sequence, ModelError> Sequence::erase_region(ItemId id) const {
    auto regions = data_->regions;
    const auto found = std::find_if(regions.begin(), regions.end(),
                                    [id](const SequenceRegion& region) { return region.id == id; });
    if (found == regions.end())
        return fail<Sequence>(ModelErrorCode::MissingItem, id, data_->id);
    regions.erase(found);
    return with_annotations(data_->markers, std::move(regions));
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
             std::move(tracks), data_->track_id_index, data_->markers, data_->regions,
             data_->chord_scale_lane, data_->groove}))));
}

const GrooveTemplate& Sequence::groove() const noexcept {
    return data_->groove;
}

const ChordScaleLane& Sequence::chord_scale_lane() const noexcept {
    return data_->chord_scale_lane;
}

Sequence Sequence::with_chord_scale_lane(ChordScaleLane lane) const {
    // The lane validated its own ordering at construction and names no
    // identities, so replacing it cannot invalidate tracks, clips, the
    // annotations, or the sequence's duration — the swap is total and cannot
    // fail.
    return Sequence(std::make_shared<const Data>(
        Data{data_->id, data_->name, data_->musical_duration, data_->absolute_duration,
             data_->tracks, data_->track_id_index, data_->markers, data_->regions,
             std::move(lane), data_->groove}));
}

Sequence Sequence::with_groove(GrooveTemplate groove) const {
    // Same reasoning as the lane swap: a groove validated its own ranges at
    // construction and names no identities, so it cannot invalidate anything
    // the sequence owns.
    return Sequence(std::make_shared<const Data>(
        Data{data_->id, data_->name, data_->musical_duration, data_->absolute_duration,
             data_->tracks, data_->track_id_index, data_->markers, data_->regions,
             data_->chord_scale_lane, std::move(groove)}));
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
    std::optional<SessionStart> session_start;
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
            case ItemKind::Marker:
            case ItemKind::Region:
                return location.sequence_id.valid() && location.sequence_id != entry.item &&
                       location.track_id == invalid && location.clip_id == invalid;
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
            case ItemKind::Marker:
            case ItemKind::Region:
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
    static_assert(kClipContentAlternativeCount == 5,
                  "ClipContent gained an alternative: this scan admits a project only when "
                  "every asset a clip names exists and is in range, and it reaches assets "
                  "through MediaRef alone. Content that names an asset any other way is "
                  "admitted unchecked here. Decide how the new content reaches assets "
                  "before widening the variant.");
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
            if (track.freeze()) {
                if (const auto error = validate_media_ref(track.freeze()->media, track.id()))
                    return fail<Project>(error->code, error->item, error->related_item);
                const auto asset = std::lower_bound(
                    input.assets.begin(), input.assets.end(), track.freeze()->media.asset_id,
                    [](const MediaAsset& candidate, ItemId id) { return candidate.id < id; });
                if (asset == input.assets.end() ||
                    asset->sample_rate.normalized() != track.freeze()->sample_rate.normalized())
                    return fail<Project>(ModelErrorCode::IncompatibleSampleRate, track.id(),
                                         track.freeze()->media.asset_id);
            }
        }
    }
    auto identities = detail::IdentityDirectory::from_sorted_entries(identity_entries);
    return runtime::Result<Project, ModelError>(runtime::Ok(
        Project(std::make_shared<const Data>(Data{.id = input.id,
                                                  .name = std::move(input.name),
                                                  .next_item_id = input.next_item_id,
                                                  .root_sequence_id = input.root_sequence_id,
                                                  .assets = std::move(input.assets),
                                                  .sequences = std::move(input.sequences),
                                                  .tempo_map = std::move(input.tempo_map),
                                                  .meter_map = std::move(input.meter_map),
                                                  .session_start = input.session_start,
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
const std::optional<SessionStart>& Project::session_start() const noexcept {
    return data_->session_start;
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
    return runtime::Result<Project, ModelError>(runtime::Ok(
        Project(std::make_shared<const Data>(Data{.id = data_->id,
                                                  .name = data_->name,
                                                  .next_item_id = next,
                                                  .root_sequence_id = data_->root_sequence_id,
                                                  .assets = std::move(assets),
                                                  .sequences = data_->sequences,
                                                  .tempo_map = data_->tempo_map,
                                                  .meter_map = data_->meter_map,
                                                  .session_start = data_->session_start,
                                                  .identities = std::move(identities)}))));
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
    static_assert(kClipContentAlternativeCount == 5,
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
    return runtime::Result<Project, ModelError>(runtime::Ok(
        Project(std::make_shared<const Data>(Data{.id = data_->id,
                                                  .name = data_->name,
                                                  .next_item_id = data_->next_item_id,
                                                  .root_sequence_id = data_->root_sequence_id,
                                                  .assets = std::move(assets),
                                                  .sequences = data_->sequences,
                                                  .tempo_map = data_->tempo_map,
                                                  .meter_map = data_->meter_map,
                                                  .session_start = data_->session_start,
                                                  .identities = std::move(identities)}))));
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
