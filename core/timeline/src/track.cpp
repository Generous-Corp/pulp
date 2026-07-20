#include <pulp/timeline/model.hpp>

#include "automation_document_internal.hpp"

#include <algorithm>

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, ModelError> fail(ModelErrorCode code, ItemId item = {}, ItemId related = {}) {
    return runtime::Err(ModelError{code, item, related});
}

runtime::Result<ClipLane, ModelError>
track_lane_result(runtime::Result<ClipLane, ModelError> result, ItemId track_id) {
    if (result)
        return result;
    auto error = result.error();
    if (error.code == ModelErrorCode::MixedTimeAnchors ||
        error.code == ModelErrorCode::IncompatibleSampleRate)
        error.item = track_id;
    return runtime::Err(error);
}

template <typename ClipRange>
std::vector<ItemId> non_automation_ids(ItemId track_id, std::span<const DevicePlacement> devices,
                                       const ClipRange& clips) {
    std::vector<ItemId> ids{track_id};
    ids.reserve(1 + devices.size() + clips.size());
    for (const auto& device : devices)
        ids.push_back(device.id);
    for (const auto& clip : clips) {
        ids.push_back(clip.id());
        if (const auto* notes = std::get_if<NoteContent>(&clip.content()))
            for (const auto& note : notes->notes())
                ids.push_back(note.id);
    }
    return ids;
}

std::shared_ptr<const std::vector<ItemId>>
canonical_automation_owned_ids(std::span<const AutomationLane> lanes) {
    std::vector<ItemId> ids;
    detail::append_automation_owned_ids(lanes, ids);
    std::sort(ids.begin(), ids.end());
    return std::make_shared<const std::vector<ItemId>>(std::move(ids));
}

std::optional<ItemId>
automation_identity_collision(const Clip& clip,
                              std::span<const ItemId> automation_owned_ids) noexcept {
    if (std::binary_search(automation_owned_ids.begin(), automation_owned_ids.end(), clip.id()))
        return clip.id();
    if (const auto* notes = std::get_if<NoteContent>(&clip.content()))
        for (const auto& note : notes->notes())
            if (std::binary_search(automation_owned_ids.begin(), automation_owned_ids.end(),
                                   note.id))
                return note.id;
    return std::nullopt;
}

} // namespace

struct Track::Data {
    ItemId id;
    std::string name;
    ClipLane arrangement_lane;
    std::shared_ptr<const std::vector<DevicePlacement>> device_chain;
    std::shared_ptr<const std::vector<AutomationLane>> automation_lanes;
    std::shared_ptr<const std::vector<ItemId>> automation_owned_ids;
};

runtime::Result<Track, ModelError> Track::create(ItemId id, std::string name,
                                                 std::vector<Clip> clips) {
    return create(TrackInput{.id = id, .name = std::move(name), .clips = std::move(clips)});
}

runtime::Result<Track, ModelError> Track::create(TrackInput input) {
    if (!input.id.valid())
        return fail<Track>(ModelErrorCode::InvalidItemId, input.id);
    std::vector<ItemId> device_ids;
    device_ids.reserve(input.device_chain.size());
    for (const auto& placement : input.device_chain) {
        if (!placement.valid())
            return fail<Track>(ModelErrorCode::InvalidItemId, placement.id);
        device_ids.push_back(placement.id);
    }
    std::sort(device_ids.begin(), device_ids.end());
    if (const auto duplicate = std::adjacent_find(device_ids.begin(), device_ids.end());
        duplicate != device_ids.end())
        return fail<Track>(ModelErrorCode::DuplicateItemId, *duplicate);
    auto lane = track_lane_result(ClipLane::create(std::move(input.clips)), input.id);
    if (!lane)
        return runtime::Err(lane.error());
    std::sort(
        input.automation_lanes.begin(), input.automation_lanes.end(),
        [](const AutomationLane& lhs, const AutomationLane& rhs) { return lhs.id() < rhs.id(); });
    const auto other_ids = non_automation_ids(input.id, input.device_chain, lane->clips());
    if (const auto error = detail::validate_attached_automation(input.automation_lanes,
                                                                input.device_chain, other_ids))
        return fail<Track>(error->code, error->item, error->related_item);
    auto device_chain =
        std::make_shared<const std::vector<DevicePlacement>>(std::move(input.device_chain));
    auto automation_lanes =
        std::make_shared<const std::vector<AutomationLane>>(std::move(input.automation_lanes));
    auto automation_owned_ids = canonical_automation_owned_ids(*automation_lanes);
    return runtime::Ok(Track(std::make_shared<const Data>(
        Data{input.id, std::move(input.name), std::move(lane).value(), std::move(device_chain),
             std::move(automation_lanes), std::move(automation_owned_ids)})));
}

runtime::Result<Track, ModelError> Track::replace_clip(Clip replacement) const {
    if (!find_clip(replacement.id()))
        return fail<Track>(ModelErrorCode::InvalidItemId, replacement.id());
    if (const auto collision =
            automation_identity_collision(replacement, *data_->automation_owned_ids))
        return fail<Track>(ModelErrorCode::DuplicateItemId, *collision);
    auto lane =
        track_lane_result(data_->arrangement_lane.replace_clip(std::move(replacement)), data_->id);
    if (!lane)
        return runtime::Err(lane.error());
    return runtime::Ok(Track(std::make_shared<const Data>(
        Data{data_->id, data_->name, std::move(lane).value(), data_->device_chain,
             data_->automation_lanes, data_->automation_owned_ids})));
}

runtime::Result<Track, ModelError> Track::insert_clip(Clip clip) const {
    if (const auto collision = automation_identity_collision(clip, *data_->automation_owned_ids))
        return fail<Track>(ModelErrorCode::DuplicateItemId, *collision);
    auto lane = track_lane_result(data_->arrangement_lane.insert_clip(std::move(clip)), data_->id);
    if (!lane)
        return runtime::Err(lane.error());
    return runtime::Ok(Track(std::make_shared<const Data>(
        Data{data_->id, data_->name, std::move(lane).value(), data_->device_chain,
             data_->automation_lanes, data_->automation_owned_ids})));
}

runtime::Result<Track, ModelError> Track::erase_clip(ItemId id) const {
    auto lane = data_->arrangement_lane.erase_clip(id);
    if (!lane)
        return runtime::Err(lane.error());
    return runtime::Ok(Track(std::make_shared<const Data>(
        Data{data_->id, data_->name, std::move(lane).value(), data_->device_chain,
             data_->automation_lanes, data_->automation_owned_ids})));
}

runtime::Result<Track, ModelError> Track::insert_automation_lane(AutomationLane lane) const {
    auto lanes = *data_->automation_lanes;
    const auto found = std::lower_bound(
        lanes.begin(), lanes.end(), lane.id(),
        [](const AutomationLane& candidate, ItemId id) { return candidate.id() < id; });
    if (found != lanes.end() && found->id() == lane.id())
        return fail<Track>(ModelErrorCode::DuplicateItemId, lane.id());
    lanes.insert(found, std::move(lane));
    const auto other_ids = non_automation_ids(data_->id, *data_->device_chain, clips());
    if (const auto error =
            detail::validate_attached_automation(lanes, *data_->device_chain, other_ids))
        return fail<Track>(error->code, error->item, error->related_item);
    auto storage = std::make_shared<const std::vector<AutomationLane>>(std::move(lanes));
    auto automation_owned_ids = canonical_automation_owned_ids(*storage);
    return runtime::Ok(Track(std::make_shared<const Data>(
        Data{data_->id, data_->name, data_->arrangement_lane, data_->device_chain,
             std::move(storage), std::move(automation_owned_ids)})));
}

runtime::Result<Track, ModelError> Track::erase_automation_lane(ItemId id) const {
    auto lanes = *data_->automation_lanes;
    const auto found = std::lower_bound(
        lanes.begin(), lanes.end(), id,
        [](const AutomationLane& candidate, ItemId wanted) { return candidate.id() < wanted; });
    if (found == lanes.end() || found->id() != id)
        return fail<Track>(ModelErrorCode::MissingItem, id);
    lanes.erase(found);
    auto storage = std::make_shared<const std::vector<AutomationLane>>(std::move(lanes));
    auto automation_owned_ids = canonical_automation_owned_ids(*storage);
    return runtime::Ok(Track(std::make_shared<const Data>(
        Data{data_->id, data_->name, data_->arrangement_lane, data_->device_chain,
             std::move(storage), std::move(automation_owned_ids)})));
}

ItemId Track::id() const noexcept {
    return data_->id;
}

const std::string& Track::name() const noexcept {
    return data_->name;
}

const ClipLane& Track::arrangement_lane() const noexcept {
    return data_->arrangement_lane;
}

Track::ClipView Track::clips() const noexcept {
    return arrangement_lane().clips();
}

const Clip* Track::find_clip(ItemId id) const noexcept {
    return arrangement_lane().find_clip(id);
}

std::span<const DevicePlacement> Track::device_chain() const noexcept {
    return *data_->device_chain;
}

const DevicePlacement* Track::find_device_placement(ItemId id) const noexcept {
    if (!id.valid())
        return nullptr;
    const auto found =
        std::find_if(data_->device_chain->begin(), data_->device_chain->end(),
                     [id](const DevicePlacement& placement) { return placement.id == id; });
    return found == data_->device_chain->end() ? nullptr : &*found;
}

std::span<const AutomationLane> Track::automation_lanes() const noexcept {
    return *data_->automation_lanes;
}

const AutomationLane* Track::find_automation_lane(ItemId id) const noexcept {
    if (!id.valid())
        return nullptr;
    const auto found = std::lower_bound(
        data_->automation_lanes->begin(), data_->automation_lanes->end(), id,
        [](const AutomationLane& candidate, ItemId wanted) { return candidate.id() < wanted; });
    return found != data_->automation_lanes->end() && found->id() == id ? &*found : nullptr;
}

std::size_t Track::shared_index_nodes_with(const Track& other) const {
    return arrangement_lane().shared_index_nodes_with(other.arrangement_lane());
}

bool Track::shares_storage_with(const Track& other) const noexcept {
    return data_.get() == other.data_.get();
}

TrackIndexStats Track::index_stats() noexcept {
    return ClipLane::index_stats();
}

} // namespace pulp::timeline
