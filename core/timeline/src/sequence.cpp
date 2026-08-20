#include <pulp/timeline/model.hpp>

#include "sequence_scene_internal.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, ModelError> fail(ModelErrorCode code, ItemId item = {}, ItemId related = {}) {
    return runtime::Err(ModelError{code, item, related});
}

bool marker_less(const SequenceMarker& lhs, const SequenceMarker& rhs) noexcept {
    return std::pair(lhs.position.value, lhs.id.value) <
           std::pair(rhs.position.value, rhs.id.value);
}

constexpr bool valid_section_role(SectionRole role) noexcept {
    switch (role) {
    case SectionRole::Unspecified:
    case SectionRole::Intro:
    case SectionRole::Verse:
    case SectionRole::PreChorus:
    case SectionRole::Chorus:
    case SectionRole::Bridge:
    case SectionRole::Breakdown:
    case SectionRole::Drop:
    case SectionRole::Solo:
    case SectionRole::Interlude:
    case SectionRole::Outro:
        return true;
    }
    return false;
}

bool region_less(const SequenceRegion& lhs, const SequenceRegion& rhs) noexcept {
    return std::tuple(lhs.position.value, lhs.duration.value, lhs.id.value) <
           std::tuple(rhs.position.value, rhs.duration.value, rhs.id.value);
}

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
        if (region.duration.value <= 0 ||
            !within_musical_span(region.position.value, region.duration.value, musical))
            return ModelError{ModelErrorCode::InvalidRegion, region.id, {}};
        if (!valid_section_role(region.role))
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

std::optional<ModelError> validate_track_span(
    const Track& track, const std::optional<timebase::TickDuration>& musical_duration,
    const std::optional<AbsoluteTimelineDuration>& absolute_duration, ItemId sequence_id) {
    for (const auto& clip : track.clips()) {
        if (clip.time_anchor() == ClipTimeAnchor::Musical && musical_duration &&
            (clip.start().value < 0 || clip.end().value > musical_duration->value))
            return ModelError{ModelErrorCode::InvalidDuration, clip.id(), sequence_id};
        if (clip.time_anchor() == ClipTimeAnchor::Absolute && absolute_duration &&
            (clip.absolute_sample_rate() != absolute_duration->sample_rate ||
             clip.absolute_start().value < 0 ||
             static_cast<std::uint64_t>(clip.absolute_end().value) >
                 absolute_duration->sample_count))
            return ModelError{ModelErrorCode::InvalidDuration, clip.id(), sequence_id};
    }
    return std::nullopt;
}

std::vector<ItemId> identity_track_order(std::span<const Track> tracks) {
    std::vector<ItemId> order;
    order.reserve(tracks.size());
    for (const auto& track : tracks)
        order.push_back(track.id());
    return order;
}

// A recorded authored order is a permutation of the track identities: it may
// not name an unknown track, repeat one, or leave one unplaced. `track_id_index`
// is already sorted by identity, so one merge walk reports the first offender.
std::optional<ModelError>
validate_track_order(std::vector<ItemId> named,
                     const std::vector<std::pair<ItemId, std::size_t>>& track_id_index,
                     ItemId sequence_id) {
    std::sort(named.begin(), named.end());
    const auto duplicate = std::adjacent_find(named.begin(), named.end());
    if (duplicate != named.end())
        return ModelError{ModelErrorCode::DuplicateItemId, *duplicate, sequence_id};
    std::size_t position = 0;
    for (const auto& entry : track_id_index) {
        if (position == named.size() || entry.first < named[position])
            return ModelError{ModelErrorCode::MissingItem, entry.first, sequence_id};
        if (named[position] < entry.first)
            return ModelError{ModelErrorCode::MissingItem, named[position], sequence_id};
        ++position;
    }
    if (position != named.size())
        return ModelError{ModelErrorCode::MissingItem, named[position], sequence_id};
    return std::nullopt;
}

std::vector<ItemId> canonical_sequence_refs(std::span<const Track> tracks) {
    std::vector<ItemId> result;
    for (const auto& track : tracks)
        for (const auto& clip : track.clips())
            if (const auto* reference = std::get_if<SequenceRef>(&clip.content()))
                result.push_back(reference->sequence_id);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<ItemId> canonical_sequence_refs(const Track& track) {
    return canonical_sequence_refs(std::span<const Track>(&track, 1));
}

} // namespace

// Every edit but `create` builds its successor by copying this value and naming
// the fields it changes, never by listing all of them positionally. Two of the
// fields — `track_order` and `outgoing_sequence_refs` — have the same type, so a
// positional list can transpose them and still compile.
struct Sequence::Data {
    ItemId id;
    std::string name;
    std::optional<timebase::TickDuration> musical_duration;
    std::optional<AbsoluteTimelineDuration> absolute_duration;
    std::vector<Track> tracks;
    std::vector<std::pair<ItemId, std::size_t>> track_id_index;
    // Authored top-to-bottom order, always naming every track exactly once.
    // Held beside `tracks` so a reorder never disturbs the identity order the
    // compile, census, and render paths index.
    std::vector<ItemId> track_order;
    std::vector<SequenceMarker> markers;
    std::vector<SequenceRegion> regions;
    ChordScaleLane chord_scale_lane;
    DynamicsLane dynamics_lane;
    GrooveTemplate groove;
    std::shared_ptr<const detail::LauncherStore> launcher;
    std::shared_ptr<const std::uint8_t> compile_structure;
    std::vector<ItemId> outgoing_sequence_refs;
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
    return create(SequenceInput{.id = id,
                                .name = std::move(name),
                                .musical_duration = musical_duration,
                                .absolute_duration = absolute_duration,
                                .tracks = std::move(tracks),
                                .markers = std::move(markers),
                                .regions = std::move(regions)});
}

runtime::Result<Sequence, ModelError>
Sequence::create(ItemId id, std::string name,
                 std::optional<timebase::TickDuration> musical_duration,
                 std::optional<AbsoluteTimelineDuration> absolute_duration,
                 std::vector<Track> tracks, std::vector<SequenceMarker> markers,
                 std::vector<SequenceRegion> regions, ChordScaleLane chord_scale_lane) {
    return create(SequenceInput{.id = id,
                                .name = std::move(name),
                                .musical_duration = musical_duration,
                                .absolute_duration = absolute_duration,
                                .tracks = std::move(tracks),
                                .markers = std::move(markers),
                                .regions = std::move(regions),
                                .chord_scale_lane = std::move(chord_scale_lane)});
}

runtime::Result<Sequence, ModelError> Sequence::create(SequenceInput input) {
    if (!input.chord_scale_lane) {
        auto lane = ChordScaleLane::create({});
        if (!lane)
            return runtime::Err(lane.error());
        input.chord_scale_lane = std::move(lane).value();
    }
    if (!input.dynamics_lane) {
        auto lane = DynamicsLane::create({});
        if (!lane)
            return runtime::Err(lane.error());
        input.dynamics_lane = std::move(lane).value();
    }
    if (!input.groove) {
        auto groove = GrooveTemplate::create({});
        if (!groove)
            return runtime::Err(groove.error());
        input.groove = std::move(groove).value();
    }
    if (!input.id.valid())
        return fail<Sequence>(ModelErrorCode::InvalidItemId, input.id);
    if ((input.musical_duration && input.musical_duration->value < 0) ||
        (input.absolute_duration && !input.absolute_duration->sample_rate.valid()))
        return fail<Sequence>(ModelErrorCode::InvalidDuration, input.id);
    if (input.absolute_duration)
        input.absolute_duration->sample_rate = input.absolute_duration->sample_rate.normalized();
    if (const auto invalid = validate_markers(input.markers, input.musical_duration))
        return fail<Sequence>(invalid->code, invalid->item, input.id);
    if (const auto invalid = validate_regions(input.regions, input.musical_duration))
        return fail<Sequence>(invalid->code, invalid->item, input.id);
    if (const auto duplicate = duplicate_annotation_id(input.markers, input.regions))
        return fail<Sequence>(ModelErrorCode::DuplicateItemId, *duplicate, input.id);

    std::sort(input.markers.begin(), input.markers.end(), marker_less);
    std::sort(input.regions.begin(), input.regions.end(), region_less);
    std::vector<std::pair<ItemId, std::size_t>> track_id_index;
    track_id_index.reserve(input.tracks.size());
    for (std::size_t index = 0; index < input.tracks.size(); ++index) {
        track_id_index.emplace_back(input.tracks[index].id(), index);
        if (const auto invalid = validate_track_span(input.tracks[index], input.musical_duration,
                                                     input.absolute_duration, input.id))
            return runtime::Err(*invalid);
    }
    std::sort(track_id_index.begin(), track_id_index.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    const auto duplicate =
        std::adjacent_find(track_id_index.begin(), track_id_index.end(),
                           [](const auto& lhs, const auto& rhs) { return lhs.first == rhs.first; });
    if (duplicate != track_id_index.end())
        return fail<Sequence>(ModelErrorCode::DuplicateItemId, duplicate->first);
    if (!input.track_order.empty()) {
        if (const auto invalid = validate_track_order(input.track_order, track_id_index, input.id))
            return runtime::Err(*invalid);
    } else {
        input.track_order = identity_track_order(input.tracks);
    }
    auto launcher = detail::build_launcher(std::move(input.scenes), input.tracks);
    if (!launcher)
        return runtime::Err(launcher.error());
    auto outgoing_sequence_refs = canonical_sequence_refs(input.tracks);
    return runtime::Ok(Sequence(std::make_shared<const Data>(
        Data{input.id, std::move(input.name), input.musical_duration, input.absolute_duration,
             std::move(input.tracks), std::move(track_id_index), std::move(input.track_order),
             std::move(input.markers), std::move(input.regions),
             std::move(*input.chord_scale_lane), std::move(*input.dynamics_lane),
             std::move(*input.groove),
             std::move(launcher).value(), std::make_shared<const std::uint8_t>(0),
             std::move(outgoing_sequence_refs)})));
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
std::span<const ItemId> Sequence::track_order() const noexcept {
    return data_->track_order;
}
std::span<const ItemId> Sequence::outgoing_sequence_refs() const noexcept {
    return data_->outgoing_sequence_refs;
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

std::size_t Sequence::SceneView::size() const noexcept {
    return detail::launcher_scene_count(*store_);
}
const Scene& Sequence::SceneView::operator[](std::size_t index) const noexcept {
    auto current = detail::launcher_head(*store_);
    while (index--)
        current = detail::launcher_next_scene(*store_, current);
    return *detail::launcher_find_scene(*store_, current);
}
Sequence::SceneView::Iterator Sequence::SceneView::begin() const noexcept {
    return Iterator(store_, detail::launcher_head(*store_));
}
Sequence::SceneView::Iterator Sequence::SceneView::end() const noexcept {
    return Iterator(store_, {});
}
const Scene& Sequence::SceneView::Iterator::operator*() const noexcept {
    return *detail::launcher_find_scene(*store_, current_);
}
const Scene* Sequence::SceneView::Iterator::operator->() const noexcept {
    return &operator*();
}
Sequence::SceneView::Iterator& Sequence::SceneView::Iterator::operator++() noexcept {
    current_ = detail::launcher_next_scene(*store_, current_);
    return *this;
}
Sequence::SceneView Sequence::scenes() const noexcept {
    return SceneView(data_->launcher);
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
const Scene* Sequence::find_scene(ItemId id) const noexcept {
    return detail::launcher_find_scene(*data_->launcher, id);
}
const Slot* Sequence::find_slot(ItemId id) const noexcept {
    return detail::launcher_find_slot(*data_->launcher, id);
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
    std::sort(markers.begin(), markers.end(), marker_less);
    std::sort(regions.begin(), regions.end(), region_less);
    auto next = *data_;
    next.markers = std::move(markers);
    next.regions = std::move(regions);
    return runtime::Ok(Sequence(std::make_shared<const Data>(std::move(next))));
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

runtime::Result<Sequence, ModelError>
Sequence::insert_scene(Scene scene, std::optional<ItemId> before_scene_id) const {
    if (before_scene_id && !find_scene(*before_scene_id))
        return fail<Sequence>(ModelErrorCode::MissingItem, *before_scene_id, data_->id);
    auto launcher = detail::insert_scene_store(data_->launcher, std::move(scene), before_scene_id,
                                               data_->tracks);
    if (!launcher)
        return runtime::Err(launcher.error());
    auto next = *data_;
    next.launcher = std::move(launcher).value();
    return runtime::Ok(Sequence(std::make_shared<const Data>(std::move(next))));
}
runtime::Result<Sequence, ModelError> Sequence::erase_scene(ItemId id) const {
    auto erased = detail::SequenceEditAccess::erase_scene(*this, id);
    if (!erased)
        return runtime::Err(erased.error());
    return runtime::Ok(std::move(erased).value().sequence);
}
runtime::Result<Sequence, ModelError>
Sequence::insert_slot(ItemId scene_id, Slot slot, std::optional<ItemId> before_slot_id) const {
    const auto* scene = find_scene(scene_id);
    if (!scene)
        return fail<Sequence>(ModelErrorCode::MissingItem, scene_id, data_->id);
    if (before_slot_id && !scene->slots.find(*before_slot_id))
        return fail<Sequence>(ModelErrorCode::MissingItem, *before_slot_id, scene_id);
    auto launcher = detail::insert_slot_store(data_->launcher, scene_id, std::move(slot),
                                              before_slot_id, data_->tracks);
    if (!launcher)
        return runtime::Err(launcher.error());
    auto next = *data_;
    next.launcher = std::move(launcher).value();
    return runtime::Ok(Sequence(std::make_shared<const Data>(std::move(next))));
}
runtime::Result<Sequence, ModelError> Sequence::erase_slot(ItemId scene_id, ItemId slot_id) const {
    auto erased = detail::SequenceEditAccess::erase_slot(*this, scene_id, slot_id);
    if (!erased)
        return runtime::Err(erased.error());
    return runtime::Ok(std::move(erased).value().sequence);
}

runtime::Result<detail::SequenceSceneEraseResult, ModelError>
detail::SequenceEditAccess::erase_scene(const Sequence& source, ItemId id) {
    const auto* scene = source.find_scene(id);
    if (!scene)
        return fail<SequenceSceneEraseResult>(ModelErrorCode::MissingItem, id, source.id());
    const Scene removed = *scene;
    auto erased = erase_scene_store(source.data_->launcher, id);
    if (!erased)
        return runtime::Err(erased.error());
    auto result = std::move(erased).value();
    auto next = *source.data_;
    next.launcher = std::move(result.store);
    auto sequence = Sequence(std::make_shared<const Sequence::Data>(std::move(next)));
    return runtime::Ok(SequenceSceneEraseResult{
        std::move(sequence), removed,
        result.following.valid() ? std::optional<ItemId>{result.following} : std::nullopt});
}

runtime::Result<detail::SequenceSlotEraseResult, ModelError>
detail::SequenceEditAccess::erase_slot(const Sequence& source, ItemId scene_id, ItemId slot_id) {
    if (!source.find_scene(scene_id))
        return fail<SequenceSlotEraseResult>(ModelErrorCode::MissingItem, scene_id, source.id());
    const auto* slot = source.find_slot(slot_id);
    if (!slot)
        return fail<SequenceSlotEraseResult>(ModelErrorCode::MissingItem, slot_id, scene_id);
    const Slot removed = *slot;
    auto erased = erase_slot_store(source.data_->launcher, scene_id, slot_id);
    if (!erased)
        return runtime::Err(erased.error());
    auto result = std::move(erased).value();
    auto next = *source.data_;
    next.launcher = std::move(result.store);
    auto sequence = Sequence(std::make_shared<const Sequence::Data>(std::move(next)));
    return runtime::Ok(SequenceSlotEraseResult{
        std::move(sequence), removed,
        result.following.valid() ? std::optional<ItemId>{result.following} : std::nullopt});
}

runtime::Result<Sequence, ModelError>
Sequence::insert_track(Track track, std::optional<ItemId> before_track_id) const {
    const ItemId track_id = track.id();
    if (!track_id.valid())
        return fail<Sequence>(ModelErrorCode::InvalidItemId, track_id, data_->id);
    if (before_track_id && !find_track(*before_track_id))
        return fail<Sequence>(ModelErrorCode::MissingItem, *before_track_id, data_->id);
    const auto entry = std::lower_bound(
        data_->track_id_index.begin(), data_->track_id_index.end(), track_id,
        [](const auto& candidate, ItemId wanted) { return candidate.first < wanted; });
    if (entry != data_->track_id_index.end() && entry->first == track_id)
        return fail<Sequence>(ModelErrorCode::DuplicateItemId, track_id, data_->id);
    if (const auto invalid = validate_track_span(track, data_->musical_duration,
                                                 data_->absolute_duration, data_->id))
        return runtime::Err(*invalid);

    const auto index_position = std::distance(data_->track_id_index.begin(), entry);

    auto next = *data_;
    const auto placement =
        before_track_id
            ? std::find(next.track_order.begin(), next.track_order.end(), *before_track_id)
            : next.track_order.end();
    next.track_order.insert(placement, track_id);
    // Appending leaves every existing identity-order index valid, so only the
    // new track needs an index entry.
    next.tracks.push_back(std::move(track));
    next.track_id_index.insert(next.track_id_index.begin() + index_position,
                               {track_id, next.tracks.size() - 1});
    next.outgoing_sequence_refs = canonical_sequence_refs(next.tracks);
    next.compile_structure = std::make_shared<const std::uint8_t>(0);
    return runtime::Ok(Sequence(std::make_shared<const Data>(std::move(next))));
}

runtime::Result<Sequence, ModelError>
Sequence::move_track(ItemId track_id, std::optional<ItemId> before_track_id) const {
    // Membership is read from the order this edit rewrites, not from the
    // identity index, so the position found and the position erased cannot
    // disagree even if the two ever drifted.
    const auto& order = data_->track_order;
    const auto placed = std::find(order.begin(), order.end(), track_id);
    if (placed == order.end())
        return fail<Sequence>(ModelErrorCode::MissingItem, track_id, data_->id);
    if (before_track_id) {
        // Naming the moved track as its own destination describes no position:
        // the track is lifted out before the destination is located, so the
        // request would silently land at the end instead.
        if (*before_track_id == track_id)
            return fail<Sequence>(ModelErrorCode::InvalidItemId, track_id, data_->id);
        if (std::find(order.begin(), order.end(), *before_track_id) == order.end())
            return fail<Sequence>(ModelErrorCode::MissingItem, *before_track_id, data_->id);
    }

    // Authored order is the only field a reorder may touch. `tracks`, the
    // identity index, and the outgoing-reference index all stay put, and the
    // compile-structure token carries over so a reorder does not invalidate a
    // compiled program that never observed display order.
    auto next = *data_;
    next.track_order.erase(next.track_order.begin() + std::distance(order.begin(), placed));
    const auto placement =
        before_track_id
            ? std::find(next.track_order.begin(), next.track_order.end(), *before_track_id)
            : next.track_order.end();
    next.track_order.insert(placement, track_id);
    return runtime::Ok(Sequence(std::make_shared<const Data>(std::move(next))));
}

runtime::Result<Sequence, ModelError> Sequence::erase_track(ItemId id) const {
    auto erased = detail::SequenceEditAccess::erase_track(*this, id);
    if (!erased)
        return runtime::Err(erased.error());
    return runtime::Ok(std::move(erased).value().sequence);
}

runtime::Result<detail::SequenceTrackEraseResult, ModelError>
detail::SequenceEditAccess::erase_track(const Sequence& source, ItemId id) {
    const auto& index = source.data_->track_id_index;
    const auto entry = std::lower_bound(
        index.begin(), index.end(), id,
        [](const auto& candidate, ItemId wanted) { return candidate.first < wanted; });
    if (entry == index.end() || entry->first != id)
        return fail<SequenceTrackEraseResult>(ModelErrorCode::MissingItem, id, source.id());
    const std::size_t position = entry->second;
    const Track removed = source.data_->tracks[position];
    // A launcher slot sources a clip by identity; dropping the track that owns
    // one would leave the slot pointing outside the sequence.
    for (const auto& clip : removed.clips())
        if (const auto slot = launcher_clip_source(*source.data_->launcher, clip.id()))
            return fail<SequenceTrackEraseResult>(ModelErrorCode::MissingItem, clip.id(), *slot);

    const auto index_position = std::distance(index.begin(), entry);

    auto next = *source.data_;
    next.tracks.erase(next.tracks.begin() + static_cast<std::ptrdiff_t>(position));
    next.track_id_index.erase(next.track_id_index.begin() + index_position);
    for (auto& remaining : next.track_id_index)
        if (remaining.second > position)
            --remaining.second;

    const auto placed = std::find(next.track_order.begin(), next.track_order.end(), id);
    std::optional<ItemId> following;
    if (placed != next.track_order.end()) {
        const auto after = std::next(placed);
        if (after != next.track_order.end())
            following = *after;
        next.track_order.erase(placed);
    }

    next.outgoing_sequence_refs = canonical_sequence_refs(next.tracks);
    next.compile_structure = std::make_shared<const std::uint8_t>(0);
    auto sequence = Sequence(std::make_shared<const Sequence::Data>(std::move(next)));
    return runtime::Ok(SequenceTrackEraseResult{std::move(sequence), removed, following});
}

runtime::Result<Sequence, ModelError> Sequence::replace_track(Track track) const {
    const auto found =
        std::lower_bound(data_->track_id_index.begin(), data_->track_id_index.end(), track.id(),
                         [](const auto& entry, ItemId wanted) { return entry.first < wanted; });
    if (found == data_->track_id_index.end() || found->first != track.id())
        return fail<Sequence>(ModelErrorCode::MissingItem, track.id(), data_->id);
    if (const auto invalid = validate_track_span(track, data_->musical_duration,
                                                 data_->absolute_duration, data_->id))
        return runtime::Err(*invalid);
    const auto& previous = data_->tracks[found->second];
    if (!previous.shares_clip_membership_with(track)) {
        for (const auto& clip : previous.clips()) {
            if (track.find_clip(clip.id()))
                continue;
            if (const auto source = detail::launcher_clip_source(*data_->launcher, clip.id()))
                return fail<Sequence>(ModelErrorCode::MissingItem, clip.id(), *source);
        }
    }
    const auto previous_references = canonical_sequence_refs(previous);
    const auto replacement_references = canonical_sequence_refs(track);
    const bool topology_unchanged = previous_references == replacement_references;
    const bool compile_structure_unchanged = previous.shares_compile_structure_with(track);
    auto next = *data_;
    next.tracks[found->second] = std::move(track);
    if (!topology_unchanged)
        next.outgoing_sequence_refs = canonical_sequence_refs(next.tracks);
    if (!compile_structure_unchanged)
        next.compile_structure = std::make_shared<const std::uint8_t>(0);
    return runtime::Ok(Sequence(std::make_shared<const Data>(std::move(next))));
}

const GrooveTemplate& Sequence::groove() const noexcept {
    return data_->groove;
}
const ChordScaleLane& Sequence::chord_scale_lane() const noexcept {
    return data_->chord_scale_lane;
}
Sequence Sequence::with_chord_scale_lane(ChordScaleLane lane) const {
    auto next = *data_;
    next.chord_scale_lane = std::move(lane);
    return Sequence(std::make_shared<const Data>(std::move(next)));
}
const DynamicsLane& Sequence::dynamics_lane() const noexcept {
    return data_->dynamics_lane;
}
Sequence Sequence::with_dynamics_lane(DynamicsLane lane) const {
    auto next = *data_;
    next.dynamics_lane = std::move(lane);
    return Sequence(std::make_shared<const Data>(std::move(next)));
}
Sequence Sequence::with_groove(GrooveTemplate groove) const {
    auto next = *data_;
    next.groove = std::move(groove);
    return Sequence(std::make_shared<const Data>(std::move(next)));
}

bool Sequence::shares_launcher_storage_with(const Sequence& other) const noexcept {
    return data_->launcher == other.data_->launcher;
}
bool Sequence::shares_storage_with(const Sequence& other) const noexcept {
    return data_ == other.data_;
}
bool Sequence::shares_compile_structure_with(const Sequence& other) const noexcept {
    return data_->compile_structure == other.data_->compile_structure;
}
LauncherIndexStats Sequence::launcher_index_stats() noexcept {
    return detail::launcher_index_stats();
}

std::size_t Sequence::shared_launcher_nodes_with(const Sequence& other) const {
    return detail::shared_launcher_nodes(*data_->launcher, *other.data_->launcher);
}

} // namespace pulp::timeline
