#include <pulp/timeline/model.hpp>

#include <algorithm>
#include <limits>
#include <tuple>

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, ModelError> fail(ModelErrorCode code, ItemId item = {}, ItemId related = {}) {
    return runtime::Result<T, ModelError>(runtime::Err(ModelError{code, item, related}));
}

std::int64_t point_scalar(const SequencePoint& point) noexcept {
    if (const auto* musical = std::get_if<MusicalSequencePoint>(&point))
        return musical->position.value;
    return std::get<AbsoluteSequencePoint>(point).position.value;
}

std::int64_t range_start_scalar(const SequenceRange& range) noexcept {
    if (const auto* musical = std::get_if<MusicalSequenceRange>(&range))
        return musical->start.value;
    return std::get<AbsoluteSequenceRange>(range).start.value;
}

bool marker_less(const SequenceMarker& lhs, const SequenceMarker& rhs) noexcept {
    return std::tuple(lhs.point.index(), point_scalar(lhs.point), lhs.id.value) <
           std::tuple(rhs.point.index(), point_scalar(rhs.point), rhs.id.value);
}

bool region_less(const SequenceRegion& lhs, const SequenceRegion& rhs) noexcept {
    return std::tuple(lhs.range.index(), range_start_scalar(lhs.range), lhs.id.value) <
           std::tuple(rhs.range.index(), range_start_scalar(rhs.range), rhs.id.value);
}

bool valid_marker(SequenceMarker& marker, std::optional<timebase::TickDuration> musical_duration,
                  std::optional<AbsoluteTimelineDuration> absolute_duration) noexcept {
    if (!marker.id.valid())
        return false;
    if (auto* musical = std::get_if<MusicalSequencePoint>(&marker.point))
        return musical->position.value >= 0 &&
               (!musical_duration || musical->position.value <= musical_duration->value);
    auto& absolute = std::get<AbsoluteSequencePoint>(marker.point);
    if (absolute.position.value < 0 || !absolute.sample_rate.valid())
        return false;
    absolute.sample_rate = absolute.sample_rate.normalized();
    return !absolute_duration ||
           (absolute.sample_rate == absolute_duration->sample_rate &&
            static_cast<std::uint64_t>(absolute.position.value) <= absolute_duration->sample_count);
}

bool valid_region(SequenceRegion& region, std::optional<timebase::TickDuration> musical_duration,
                  std::optional<AbsoluteTimelineDuration> absolute_duration) noexcept {
    if (!region.id.valid())
        return false;
    if (auto* musical = std::get_if<MusicalSequenceRange>(&region.range)) {
        if (musical->start.value < 0 || musical->duration.value <= 0 ||
            musical->start.value >
                std::numeric_limits<std::int64_t>::max() - musical->duration.value)
            return false;
        return !musical_duration ||
               musical->start.value + musical->duration.value <= musical_duration->value;
    }
    auto& absolute = std::get<AbsoluteSequenceRange>(region.range);
    if (absolute.start.value < 0 || absolute.sample_count == 0 ||
        !absolute.sample_rate.valid() ||
        static_cast<std::uint64_t>(absolute.start.value) >
            std::numeric_limits<std::uint64_t>::max() - absolute.sample_count)
        return false;
    absolute.sample_rate = absolute.sample_rate.normalized();
    return !absolute_duration ||
           (absolute.sample_rate == absolute_duration->sample_rate &&
            static_cast<std::uint64_t>(absolute.start.value) + absolute.sample_count <=
                absolute_duration->sample_count);
}

template <typename T>
const T* find_id(std::span<const T> values, ItemId id) noexcept {
    const auto found = std::find_if(values.begin(), values.end(),
                                    [id](const T& value) { return value.id == id; });
    return found == values.end() ? nullptr : &*found;
}

} // namespace

struct Sequence::Data {
    ItemId id;
    std::string name;
    std::optional<timebase::TickDuration> musical_duration;
    std::optional<AbsoluteTimelineDuration> absolute_duration;
    std::vector<Track> tracks;
    std::vector<std::pair<ItemId, std::size_t>> track_id_index;
    std::shared_ptr<const std::vector<SequenceMarker>> markers;
    std::shared_ptr<const std::vector<SequenceRegion>> regions;
};

runtime::Result<Sequence, ModelError>
Sequence::create(ItemId id, std::string name, std::optional<timebase::TickDuration> duration,
                 std::vector<Track> tracks) {
    return create(id, std::move(name), duration, std::nullopt, std::move(tracks));
}

runtime::Result<Sequence, ModelError> Sequence::create(
    ItemId id, std::string name, std::optional<timebase::TickDuration> musical_duration,
    std::optional<AbsoluteTimelineDuration> absolute_duration, std::vector<Track> tracks) {
    return create(SequenceInput{id, std::move(name), musical_duration, absolute_duration,
                                std::move(tracks), {}, {}});
}

runtime::Result<Sequence, ModelError> Sequence::create(SequenceInput input) {
    if (!input.id.valid())
        return fail<Sequence>(ModelErrorCode::InvalidItemId, input.id);
    if ((input.musical_duration && input.musical_duration->value < 0) ||
        (input.absolute_duration && !input.absolute_duration->sample_rate.valid()))
        return fail<Sequence>(ModelErrorCode::InvalidDuration, input.id);
    if (input.absolute_duration)
        input.absolute_duration->sample_rate = input.absolute_duration->sample_rate.normalized();
    std::vector<std::pair<ItemId, std::size_t>> by_id;
    by_id.reserve(input.tracks.size());
    std::vector<ItemId> direct_ids{input.id};
    direct_ids.reserve(1 + input.tracks.size() + input.markers.size() + input.regions.size());
    for (std::size_t index = 0; index < input.tracks.size(); ++index) {
        by_id.emplace_back(input.tracks[index].id(), index);
        direct_ids.push_back(input.tracks[index].id());
        for (const auto& clip : input.tracks[index].clips()) {
            if (clip.time_anchor() == ClipTimeAnchor::Musical && input.musical_duration &&
                (clip.start().value < 0 || clip.end().value > input.musical_duration->value))
                return fail<Sequence>(ModelErrorCode::InvalidDuration, clip.id(), input.id);
            if (clip.time_anchor() == ClipTimeAnchor::Absolute && input.absolute_duration &&
                (clip.absolute_sample_rate() != input.absolute_duration->sample_rate ||
                 clip.absolute_start().value < 0 ||
                 static_cast<std::uint64_t>(clip.absolute_end().value) >
                     input.absolute_duration->sample_count))
                return fail<Sequence>(ModelErrorCode::InvalidDuration, clip.id(), input.id);
        }
    }
    std::sort(by_id.begin(), by_id.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    if (const auto duplicate = std::adjacent_find(
            by_id.begin(), by_id.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.first == rhs.first; });
        duplicate != by_id.end())
        return fail<Sequence>(ModelErrorCode::DuplicateItemId, duplicate->first);

    for (auto& marker : input.markers) {
        if (!valid_marker(marker, input.musical_duration, input.absolute_duration))
            return fail<Sequence>(marker.id.valid() ? ModelErrorCode::InvalidDuration
                                                    : ModelErrorCode::InvalidItemId,
                                  marker.id, input.id);
        direct_ids.push_back(marker.id);
    }
    for (auto& region : input.regions) {
        if (!valid_region(region, input.musical_duration, input.absolute_duration))
            return fail<Sequence>(region.id.valid() ? ModelErrorCode::InvalidDuration
                                                    : ModelErrorCode::InvalidItemId,
                                  region.id, input.id);
        direct_ids.push_back(region.id);
    }
    std::optional<timebase::RationalRate> annotation_rate;
    const auto accept_rate = [&](timebase::RationalRate rate) {
        if (!annotation_rate)
            annotation_rate = rate;
        return *annotation_rate == rate;
    };
    for (const auto& marker : input.markers)
        if (const auto* absolute = std::get_if<AbsoluteSequencePoint>(&marker.point);
            absolute && !accept_rate(absolute->sample_rate))
            return fail<Sequence>(ModelErrorCode::IncompatibleSampleRate, marker.id, input.id);
    for (const auto& region : input.regions)
        if (const auto* absolute = std::get_if<AbsoluteSequenceRange>(&region.range);
            absolute && !accept_rate(absolute->sample_rate))
            return fail<Sequence>(ModelErrorCode::IncompatibleSampleRate, region.id, input.id);
    std::sort(direct_ids.begin(), direct_ids.end());
    if (const auto duplicate = std::adjacent_find(direct_ids.begin(), direct_ids.end());
        duplicate != direct_ids.end())
        return fail<Sequence>(ModelErrorCode::DuplicateItemId, *duplicate, input.id);
    std::sort(input.markers.begin(), input.markers.end(), marker_less);
    std::sort(input.regions.begin(), input.regions.end(), region_less);

    return runtime::Ok(Sequence(std::make_shared<const Data>(Data{
        input.id, std::move(input.name), input.musical_duration, input.absolute_duration,
        std::move(input.tracks), std::move(by_id),
        std::make_shared<const std::vector<SequenceMarker>>(std::move(input.markers)),
        std::make_shared<const std::vector<SequenceRegion>>(std::move(input.regions))})));
}

ItemId Sequence::id() const noexcept { return data_->id; }
const std::string& Sequence::name() const noexcept { return data_->name; }
std::optional<timebase::TickDuration> Sequence::duration() const noexcept {
    return data_->musical_duration;
}
std::optional<AbsoluteTimelineDuration> Sequence::absolute_duration() const noexcept {
    return data_->absolute_duration;
}
std::span<const Track> Sequence::tracks() const noexcept { return data_->tracks; }
std::span<const SequenceMarker> Sequence::markers() const noexcept { return *data_->markers; }
std::span<const SequenceRegion> Sequence::regions() const noexcept { return *data_->regions; }

const Track* Sequence::find_track(ItemId id) const noexcept {
    const auto found =
        std::lower_bound(data_->track_id_index.begin(), data_->track_id_index.end(), id,
                         [](const auto& entry, ItemId wanted) { return entry.first < wanted; });
    return found != data_->track_id_index.end() && found->first == id
               ? &data_->tracks[found->second]
               : nullptr;
}
const SequenceMarker* Sequence::find_marker(ItemId id) const noexcept {
    return find_id<SequenceMarker>(*data_->markers, id);
}
const SequenceRegion* Sequence::find_region(ItemId id) const noexcept {
    return find_id<SequenceRegion>(*data_->regions, id);
}

runtime::Result<Sequence, ModelError> Sequence::replace_track(Track track) const {
    const auto found =
        std::lower_bound(data_->track_id_index.begin(), data_->track_id_index.end(), track.id(),
                         [](const auto& entry, ItemId wanted) { return entry.first < wanted; });
    if (found == data_->track_id_index.end() || found->first != track.id())
        return fail<Sequence>(ModelErrorCode::MissingItem, track.id(), data_->id);
    auto tracks = data_->tracks;
    tracks[found->second] = std::move(track);
    return create(SequenceInput{data_->id, data_->name, data_->musical_duration,
                                data_->absolute_duration, std::move(tracks), *data_->markers,
                                *data_->regions});
}

runtime::Result<Sequence, ModelError> Sequence::insert_marker(SequenceMarker marker) const {
    if (find_marker(marker.id) || find_region(marker.id) || find_track(marker.id))
        return fail<Sequence>(ModelErrorCode::DuplicateItemId, marker.id, data_->id);
    auto values = *data_->markers;
    values.push_back(std::move(marker));
    return create(SequenceInput{data_->id, data_->name, data_->musical_duration,
                                data_->absolute_duration, data_->tracks, std::move(values),
                                *data_->regions});
}
runtime::Result<Sequence, ModelError> Sequence::erase_marker(ItemId id) const {
    auto values = *data_->markers;
    const auto found = std::find_if(values.begin(), values.end(),
                                    [id](const auto& value) { return value.id == id; });
    if (found == values.end())
        return fail<Sequence>(ModelErrorCode::MissingItem, id, data_->id);
    values.erase(found);
    return create(SequenceInput{data_->id, data_->name, data_->musical_duration,
                                data_->absolute_duration, data_->tracks, std::move(values),
                                *data_->regions});
}
runtime::Result<Sequence, ModelError> Sequence::replace_marker(SequenceMarker marker) const {
    auto values = *data_->markers;
    const auto found = std::find_if(values.begin(), values.end(),
                                    [&](const auto& value) { return value.id == marker.id; });
    if (found == values.end())
        return fail<Sequence>(ModelErrorCode::MissingItem, marker.id, data_->id);
    *found = std::move(marker);
    return create(SequenceInput{data_->id, data_->name, data_->musical_duration,
                                data_->absolute_duration, data_->tracks, std::move(values),
                                *data_->regions});
}
runtime::Result<Sequence, ModelError> Sequence::insert_region(SequenceRegion region) const {
    if (find_marker(region.id) || find_region(region.id) || find_track(region.id))
        return fail<Sequence>(ModelErrorCode::DuplicateItemId, region.id, data_->id);
    auto values = *data_->regions;
    values.push_back(std::move(region));
    return create(SequenceInput{data_->id, data_->name, data_->musical_duration,
                                data_->absolute_duration, data_->tracks, *data_->markers,
                                std::move(values)});
}
runtime::Result<Sequence, ModelError> Sequence::erase_region(ItemId id) const {
    auto values = *data_->regions;
    const auto found = std::find_if(values.begin(), values.end(),
                                    [id](const auto& value) { return value.id == id; });
    if (found == values.end())
        return fail<Sequence>(ModelErrorCode::MissingItem, id, data_->id);
    values.erase(found);
    return create(SequenceInput{data_->id, data_->name, data_->musical_duration,
                                data_->absolute_duration, data_->tracks, *data_->markers,
                                std::move(values)});
}
runtime::Result<Sequence, ModelError> Sequence::replace_region(SequenceRegion region) const {
    auto values = *data_->regions;
    const auto found = std::find_if(values.begin(), values.end(),
                                    [&](const auto& value) { return value.id == region.id; });
    if (found == values.end())
        return fail<Sequence>(ModelErrorCode::MissingItem, region.id, data_->id);
    *found = std::move(region);
    return create(SequenceInput{data_->id, data_->name, data_->musical_duration,
                                data_->absolute_duration, data_->tracks, *data_->markers,
                                std::move(values)});
}

bool Sequence::shares_storage_with(const Sequence& other) const noexcept {
    return data_.get() == other.data_.get();
}

} // namespace pulp::timeline
