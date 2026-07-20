#include <pulp/timeline/model.hpp>

#include <algorithm>

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, ModelError> fail(ModelErrorCode code, ItemId item = {}, ItemId related = {}) {
    return runtime::Result<T, ModelError>(runtime::Err(ModelError{code, item, related}));
}

} // namespace

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

} // namespace pulp::timeline
