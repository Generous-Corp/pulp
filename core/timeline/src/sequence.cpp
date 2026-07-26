#include <pulp/timeline/model.hpp>

#include "sequence_scene_internal.hpp"

#include <algorithm>
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

} // namespace

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
    std::shared_ptr<const detail::LauncherStore> launcher;
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
    auto launcher = detail::build_launcher(std::move(input.scenes), input.tracks);
    if (!launcher)
        return runtime::Err(launcher.error());
    return runtime::Ok(Sequence(std::make_shared<const Data>(
        Data{input.id, std::move(input.name), input.musical_duration, input.absolute_duration,
             std::move(input.tracks), std::move(track_id_index), std::move(input.markers),
             std::move(input.regions), std::move(*input.chord_scale_lane), std::move(*input.groove),
             std::move(launcher).value()})));
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
    return runtime::Ok(Sequence(std::make_shared<const Data>(
        Data{data_->id, data_->name, data_->musical_duration, data_->absolute_duration,
             data_->tracks, data_->track_id_index, std::move(markers), std::move(regions),
             data_->chord_scale_lane, data_->groove, data_->launcher})));
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
    return runtime::Ok(Sequence(std::make_shared<const Data>(
        Data{data_->id, data_->name, data_->musical_duration, data_->absolute_duration,
             data_->tracks, data_->track_id_index, data_->markers, data_->regions,
             data_->chord_scale_lane, data_->groove, std::move(launcher).value()})));
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
    return runtime::Ok(Sequence(std::make_shared<const Data>(
        Data{data_->id, data_->name, data_->musical_duration, data_->absolute_duration,
             data_->tracks, data_->track_id_index, data_->markers, data_->regions,
             data_->chord_scale_lane, data_->groove, std::move(launcher).value()})));
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
    auto sequence = Sequence(std::make_shared<const Sequence::Data>(Sequence::Data{
        source.data_->id, source.data_->name, source.data_->musical_duration,
        source.data_->absolute_duration, source.data_->tracks, source.data_->track_id_index,
        source.data_->markers, source.data_->regions, source.data_->chord_scale_lane,
        source.data_->groove, std::move(result.store)}));
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
    auto sequence = Sequence(std::make_shared<const Sequence::Data>(Sequence::Data{
        source.data_->id, source.data_->name, source.data_->musical_duration,
        source.data_->absolute_duration, source.data_->tracks, source.data_->track_id_index,
        source.data_->markers, source.data_->regions, source.data_->chord_scale_lane,
        source.data_->groove, std::move(result.store)}));
    return runtime::Ok(SequenceSlotEraseResult{
        std::move(sequence), removed,
        result.following.valid() ? std::optional<ItemId>{result.following} : std::nullopt});
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
    auto tracks = data_->tracks;
    tracks[found->second] = std::move(track);
    return runtime::Ok(Sequence(std::make_shared<const Data>(
        Data{data_->id, data_->name, data_->musical_duration, data_->absolute_duration,
             std::move(tracks), data_->track_id_index, data_->markers, data_->regions,
             data_->chord_scale_lane, data_->groove, data_->launcher})));
}

const GrooveTemplate& Sequence::groove() const noexcept {
    return data_->groove;
}
const ChordScaleLane& Sequence::chord_scale_lane() const noexcept {
    return data_->chord_scale_lane;
}
Sequence Sequence::with_chord_scale_lane(ChordScaleLane lane) const {
    return Sequence(std::make_shared<const Data>(
        Data{data_->id, data_->name, data_->musical_duration, data_->absolute_duration,
             data_->tracks, data_->track_id_index, data_->markers, data_->regions, std::move(lane),
             data_->groove, data_->launcher}));
}
Sequence Sequence::with_groove(GrooveTemplate groove) const {
    return Sequence(std::make_shared<const Data>(
        Data{data_->id, data_->name, data_->musical_duration, data_->absolute_duration,
             data_->tracks, data_->track_id_index, data_->markers, data_->regions,
             data_->chord_scale_lane, std::move(groove), data_->launcher}));
}

bool Sequence::shares_launcher_storage_with(const Sequence& other) const noexcept {
    return data_->launcher == other.data_->launcher;
}
bool Sequence::shares_storage_with(const Sequence& other) const noexcept {
    return data_ == other.data_;
}
LauncherIndexStats Sequence::launcher_index_stats() noexcept {
    return detail::launcher_index_stats();
}

std::size_t Sequence::shared_launcher_nodes_with(const Sequence& other) const {
    return detail::shared_launcher_nodes(*data_->launcher, *other.data_->launcher);
}

} // namespace pulp::timeline
