#include <pulp/timeline/model.hpp>

#include "automation_document_internal.hpp"

#include <algorithm>
#include <atomic>
#include <tuple>
#include <unordered_set>

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, ModelError> fail(ModelErrorCode code, ItemId item = {}, ItemId related = {}) {
    return runtime::Result<T, ModelError>(runtime::Err(ModelError{code, item, related}));
}

std::int64_t clip_start_scalar(const Clip& clip) noexcept {
    return clip.time_anchor() == ClipTimeAnchor::Musical ? clip.start().value
                                                         : clip.absolute_start().value;
}

std::int64_t clip_end_scalar(const Clip& clip) noexcept {
    return clip.time_anchor() == ClipTimeAnchor::Musical ? clip.end().value
                                                         : clip.absolute_end().value;
}

bool start_less(const Clip& lhs, const Clip& rhs) noexcept {
    return std::tuple(lhs.time_anchor(), clip_start_scalar(lhs), lhs.id().value) <
           std::tuple(rhs.time_anchor(), clip_start_scalar(rhs), rhs.id().value);
}

bool id_less(const Clip& lhs, const Clip& rhs) noexcept {
    return lhs.id() < rhs.id();
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

void append_take_owned_ids(std::span<const TakeLane> lanes, std::vector<ItemId>& ids) {
    for (const auto& lane : lanes) {
        ids.push_back(lane.id());
        for (const auto& take : lane.takes())
            ids.push_back(take.id());
    }
}

std::shared_ptr<const std::vector<ItemId>>
canonical_take_owned_ids(std::span<const TakeLane> lanes) {
    std::vector<ItemId> ids;
    append_take_owned_ids(lanes, ids);
    std::sort(ids.begin(), ids.end());
    return std::make_shared<const std::vector<ItemId>>(std::move(ids));
}

// Full-matrix collision check for take lanes attaching to a track: every
// take-owned id (lanes and their takes) must be valid, unique among takes, and
// disjoint from every other track-owned id. Fail-closed on any violation.
std::optional<ModelError> validate_attached_takes(std::span<const TakeLane> lanes,
                                                  std::span<const ItemId> other_ids) {
    std::vector<ItemId> take_ids;
    append_take_owned_ids(lanes, take_ids);
    for (const auto id : take_ids)
        if (!id.valid())
            return ModelError{ModelErrorCode::InvalidTake, id, {}};
    std::sort(take_ids.begin(), take_ids.end());
    if (const auto duplicate = std::adjacent_find(take_ids.begin(), take_ids.end());
        duplicate != take_ids.end())
        return ModelError{ModelErrorCode::DuplicateTake, *duplicate, {}};
    std::vector<ItemId> others(other_ids.begin(), other_ids.end());
    std::sort(others.begin(), others.end());
    for (const auto id : take_ids)
        if (std::binary_search(others.begin(), others.end(), id))
            return ModelError{ModelErrorCode::DuplicateItemId, id, {}};
    return std::nullopt;
}

std::optional<ItemId> take_identity_collision(const Clip& clip,
                                              std::span<const ItemId> take_owned_ids) noexcept {
    if (std::binary_search(take_owned_ids.begin(), take_owned_ids.end(), clip.id()))
        return clip.id();
    if (const auto* notes = std::get_if<NoteContent>(&clip.content()))
        for (const auto& note : notes->notes())
            if (std::binary_search(take_owned_ids.begin(), take_owned_ids.end(), note.id))
                return note.id;
    return std::nullopt;
}

} // namespace

static std::atomic<std::uint64_t> g_live_index_nodes{0};
static std::atomic<std::uint64_t> g_created_index_nodes{0};

struct ClipIndexNode {
    Clip clip;
    std::shared_ptr<const ClipIndexNode> left;
    std::shared_ptr<const ClipIndexNode> right;
    std::uint8_t height = 1;
    std::size_t count = 1;

    ClipIndexNode(Clip value, std::shared_ptr<const ClipIndexNode> lhs,
                  std::shared_ptr<const ClipIndexNode> rhs, std::uint8_t node_height,
                  std::size_t node_count)
        : clip(std::move(value)), left(std::move(lhs)), right(std::move(rhs)), height(node_height),
          count(node_count) {
        ++g_live_index_nodes;
        ++g_created_index_nodes;
    }
    ~ClipIndexNode() {
        --g_live_index_nodes;
    }
};

namespace {

using NodePtr = std::shared_ptr<const ClipIndexNode>;
using Less = bool (*)(const Clip&, const Clip&) noexcept;

std::uint8_t height(const NodePtr& node) noexcept {
    return node ? node->height : 0;
}
std::size_t count(const NodePtr& node) noexcept {
    return node ? node->count : 0;
}

NodePtr node(Clip clip, NodePtr left = {}, NodePtr right = {}) {
    const auto node_height = static_cast<std::uint8_t>(1 + std::max(height(left), height(right)));
    const auto node_count = 1 + count(left) + count(right);
    return std::make_shared<const ClipIndexNode>(std::move(clip), std::move(left), std::move(right),
                                                 node_height, node_count);
}

NodePtr build_balanced(std::span<const Clip> clips, std::size_t begin, std::size_t end) {
    if (begin == end)
        return {};
    const auto middle = begin + (end - begin) / 2;
    auto left = build_balanced(clips, begin, middle);
    auto right = build_balanced(clips, middle + 1, end);
    return node(clips[middle], std::move(left), std::move(right));
}

NodePtr balance(Clip clip, NodePtr left, NodePtr right) {
    if (height(left) > height(right) + 1) {
        if (height(left->left) < height(left->right)) {
            const auto pivot = left->right;
            auto rotated_left = node(left->clip, left->left, pivot->left);
            left = node(pivot->clip, std::move(rotated_left), pivot->right);
        }
        const auto pivot = left;
        auto new_right = node(std::move(clip), pivot->right, std::move(right));
        return node(pivot->clip, pivot->left, std::move(new_right));
    }
    if (height(right) > height(left) + 1) {
        if (height(right->right) < height(right->left)) {
            const auto pivot = right->left;
            auto rotated_right = node(right->clip, pivot->right, right->right);
            right = node(pivot->clip, pivot->left, std::move(rotated_right));
        }
        const auto pivot = right;
        auto new_left = node(std::move(clip), std::move(left), pivot->left);
        return node(pivot->clip, std::move(new_left), pivot->right);
    }
    return node(std::move(clip), std::move(left), std::move(right));
}

NodePtr insert(NodePtr root, Clip clip, Less less, bool& duplicate) {
    if (!root)
        return node(std::move(clip));
    if (less(clip, root->clip))
        return balance(root->clip, insert(root->left, std::move(clip), less, duplicate),
                       root->right);
    if (less(root->clip, clip))
        return balance(root->clip, root->left,
                       insert(root->right, std::move(clip), less, duplicate));
    duplicate = true;
    return root;
}

const ClipIndexNode* minimum(const NodePtr& root) noexcept {
    auto* current = root.get();
    while (current && current->left)
        current = current->left.get();
    return current;
}

NodePtr erase(NodePtr root, const Clip& key, Less less) {
    if (!root)
        return {};
    if (less(key, root->clip))
        return balance(root->clip, erase(root->left, key, less), root->right);
    if (less(root->clip, key))
        return balance(root->clip, root->left, erase(root->right, key, less));
    if (!root->left)
        return root->right;
    if (!root->right)
        return root->left;
    const auto* successor = minimum(root->right);
    return balance(successor->clip, root->left, erase(root->right, successor->clip, less));
}

const Clip* find(NodePtr root, const Clip& key, Less less) noexcept {
    while (root) {
        if (less(key, root->clip))
            root = root->left;
        else if (less(root->clip, key))
            root = root->right;
        else
            return &root->clip;
    }
    return nullptr;
}

const Clip* find_id(NodePtr root, ItemId id) noexcept {
    while (root) {
        if (id < root->clip.id())
            root = root->left;
        else if (root->clip.id() < id)
            root = root->right;
        else
            return &root->clip;
    }
    return nullptr;
}

const Clip* predecessor(NodePtr root, const Clip& key, Less less) noexcept {
    const Clip* result = nullptr;
    while (root) {
        if (less(root->clip, key)) {
            result = &root->clip;
            root = root->right;
        } else {
            root = root->left;
        }
    }
    return result;
}

const Clip* successor(NodePtr root, const Clip& key, Less less) noexcept {
    const Clip* result = nullptr;
    while (root) {
        if (less(key, root->clip)) {
            result = &root->clip;
            root = root->left;
        } else {
            root = root->right;
        }
    }
    return result;
}

const Clip& select(const NodePtr& root, std::size_t rank) noexcept {
    auto current = root;
    while (current) {
        const auto left_count = count(current->left);
        if (rank < left_count)
            current = current->left;
        else if (rank == left_count)
            return current->clip;
        else {
            rank -= left_count + 1;
            current = current->right;
        }
    }
    std::terminate();
}

bool ranges_overlap(const Clip& lhs, const Clip& rhs) noexcept {
    return lhs.time_anchor() == rhs.time_anchor() &&
           clip_start_scalar(lhs) < clip_end_scalar(rhs) &&
           clip_start_scalar(rhs) < clip_end_scalar(lhs);
}

std::optional<std::pair<ItemId, ItemId>> first_overlap(std::span<const Clip> clips) {
    for (std::size_t i = 1; i < clips.size(); ++i)
        if (ranges_overlap(clips[i - 1], clips[i]))
            return std::pair(clips[i - 1].id(), clips[i].id());
    return std::nullopt;
}

void collect_addresses(const NodePtr& root, std::unordered_set<const ClipIndexNode*>& out) {
    if (!root)
        return;
    out.insert(root.get());
    collect_addresses(root->left, out);
    collect_addresses(root->right, out);
}

std::size_t count_shared(const NodePtr& root,
                         const std::unordered_set<const ClipIndexNode*>& addresses) {
    if (!root)
        return 0;
    if (addresses.contains(root.get()))
        return root->count;
    return count_shared(root->left, addresses) + count_shared(root->right, addresses);
}

} // namespace

struct Track::Data {
    ItemId id;
    std::string name;
    NodePtr clips_by_start;
    NodePtr clips_by_id;
    std::shared_ptr<const std::vector<DevicePlacement>> device_chain;
    std::shared_ptr<const std::vector<AutomationLane>> automation_lanes;
    std::shared_ptr<const std::vector<ItemId>> automation_owned_ids;
    std::shared_ptr<const std::vector<TakeLane>> take_lanes;
    std::shared_ptr<const std::vector<ItemId>> take_owned_ids;
    bool record_armed = false;
    ItemId active_take_lane_id;
    std::optional<TrackFreeze> freeze;
};

std::optional<ModelErrorCode> track_freeze_error(const TrackFreeze& freeze) noexcept {
    if (!freeze.media.asset_id.valid() || freeze.media.source_start.value < 0 ||
        freeze.media.frame_count == 0)
        return ModelErrorCode::InvalidMediaRange;
    if (!freeze.sample_rate.valid() || freeze.sample_rate.normalized() != freeze.sample_rate)
        return ModelErrorCode::InvalidSampleRate;
    if (!freeze.render_plan_hash.valid())
        return ModelErrorCode::InvalidContentHash;
    const auto source_start = static_cast<std::uint64_t>(freeze.media.source_start.value);
    if (source_start > std::numeric_limits<std::uint64_t>::max() - freeze.media.frame_count)
        return ModelErrorCode::InvalidMediaRange;
    return std::nullopt;
}

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
    if (!input.clips.empty()) {
        const auto anchor = input.clips.front().time_anchor();
        const auto absolute_rate = input.clips.front().absolute_sample_rate();
        for (const auto& clip : input.clips) {
            if (clip.time_anchor() != anchor)
                return fail<Track>(ModelErrorCode::MixedTimeAnchors, input.id, clip.id());
            if (anchor == ClipTimeAnchor::Absolute && clip.absolute_sample_rate() != absolute_rate)
                return fail<Track>(ModelErrorCode::IncompatibleSampleRate, input.id, clip.id());
        }
    }
    auto clips_by_start = input.clips;
    std::sort(clips_by_start.begin(), clips_by_start.end(), start_less);
    std::sort(input.clips.begin(), input.clips.end(), id_less);
    if (const auto duplicate = std::adjacent_find(
            input.clips.begin(), input.clips.end(),
            [](const Clip& lhs, const Clip& rhs) { return lhs.id() == rhs.id(); });
        duplicate != input.clips.end())
        return fail<Track>(ModelErrorCode::DuplicateItemId, duplicate->id());
    if (const auto overlap = first_overlap(clips_by_start))
        return fail<Track>(ModelErrorCode::OverlappingClips, overlap->first, overlap->second);
    auto by_start = build_balanced(clips_by_start, 0, clips_by_start.size());
    auto by_id = build_balanced(input.clips, 0, input.clips.size());
    std::sort(
        input.automation_lanes.begin(), input.automation_lanes.end(),
        [](const AutomationLane& lhs, const AutomationLane& rhs) { return lhs.id() < rhs.id(); });
    const auto other_ids =
        non_automation_ids(input.id, input.device_chain, ClipView(by_start, count(by_start)));
    if (const auto error = detail::validate_attached_automation(input.automation_lanes,
                                                                input.device_chain, other_ids))
        return fail<Track>(error->code, error->item, error->related_item);
    auto device_chain =
        std::make_shared<const std::vector<DevicePlacement>>(std::move(input.device_chain));
    auto automation_lanes =
        std::make_shared<const std::vector<AutomationLane>>(std::move(input.automation_lanes));
    auto automation_owned_ids = canonical_automation_owned_ids(*automation_lanes);
    // Takes must be disjoint from every other track-owned id: reuse the same
    // non-automation id set plus the automation-owned ids computed above.
    auto take_other_ids = other_ids;
    take_other_ids.insert(take_other_ids.end(), automation_owned_ids->begin(),
                          automation_owned_ids->end());
    if (const auto error = validate_attached_takes(input.take_lanes, take_other_ids))
        return fail<Track>(error->code, error->item, error->related_item);
    std::sort(input.take_lanes.begin(), input.take_lanes.end(),
              [](const TakeLane& lhs, const TakeLane& rhs) { return lhs.id() < rhs.id(); });
    if (input.active_take_lane_id.value != 0 && !input.active_take_lane_id.valid())
        return fail<Track>(ModelErrorCode::InvalidItemId, input.active_take_lane_id, input.id);
    if (input.active_take_lane_id.valid()) {
        const auto active = std::lower_bound(
            input.take_lanes.begin(), input.take_lanes.end(), input.active_take_lane_id,
            [](const TakeLane& lane, ItemId wanted) { return lane.id() < wanted; });
        if (active == input.take_lanes.end() || active->id() != input.active_take_lane_id)
            return fail<Track>(ModelErrorCode::MissingItem, input.active_take_lane_id, input.id);
    }
    if (input.freeze) {
        if (const auto error = track_freeze_error(*input.freeze)) {
            return fail<Track>(*error, input.id, input.freeze->media.asset_id);
        }
    }
    auto take_lanes = std::make_shared<const std::vector<TakeLane>>(std::move(input.take_lanes));
    auto take_owned_ids = canonical_take_owned_ids(*take_lanes);
    return runtime::Result<Track, ModelError>(runtime::Ok(Track(
        std::make_shared<const Data>(Data{.id = input.id,
                                          .name = std::move(input.name),
                                          .clips_by_start = std::move(by_start),
                                          .clips_by_id = std::move(by_id),
                                          .device_chain = std::move(device_chain),
                                          .automation_lanes = std::move(automation_lanes),
                                          .automation_owned_ids = std::move(automation_owned_ids),
                                          .take_lanes = std::move(take_lanes),
                                          .take_owned_ids = std::move(take_owned_ids),
                                          .record_armed = input.record_armed,
                                          .active_take_lane_id = input.active_take_lane_id,
                                          .freeze = std::move(input.freeze)}))));
}

runtime::Result<Track, ModelError> Track::replace_clip(Clip replacement) const {
    const Clip probe = replacement;
    const ItemId replacement_id = replacement.id();
    const auto* old = find(data_->clips_by_id, probe, id_less);
    if (!old)
        return fail<Track>(ModelErrorCode::InvalidItemId, replacement.id());
    if (replacement.time_anchor() != old->time_anchor())
        return fail<Track>(ModelErrorCode::MixedTimeAnchors, data_->id, replacement.id());
    if (replacement.time_anchor() == ClipTimeAnchor::Absolute &&
        replacement.absolute_sample_rate() != old->absolute_sample_rate())
        return fail<Track>(ModelErrorCode::IncompatibleSampleRate, data_->id, replacement.id());
    if (const auto collision =
            automation_identity_collision(replacement, *data_->automation_owned_ids))
        return fail<Track>(ModelErrorCode::DuplicateItemId, *collision);
    if (const auto collision = take_identity_collision(replacement, *data_->take_owned_ids))
        return fail<Track>(ModelErrorCode::DuplicateItemId, *collision);
    auto by_start = erase(data_->clips_by_start, *old, start_less);
    auto by_id = erase(data_->clips_by_id, *old, id_less);
    bool duplicate = false;
    by_start = insert(std::move(by_start), replacement, start_less, duplicate);
    duplicate = false;
    by_id = insert(std::move(by_id), std::move(replacement), id_less, duplicate);
    const auto* inserted = find_id(by_id, replacement_id);
    const auto* previous = predecessor(by_start, *inserted, start_less);
    const auto* next = successor(by_start, *inserted, start_less);
    if (previous && ranges_overlap(*previous, *inserted))
        return fail<Track>(ModelErrorCode::OverlappingClips, previous->id(), inserted->id());
    if (next && ranges_overlap(*inserted, *next))
        return fail<Track>(ModelErrorCode::OverlappingClips, inserted->id(), next->id());
    auto next_data = *data_;
    next_data.clips_by_start = std::move(by_start);
    next_data.clips_by_id = std::move(by_id);
    return runtime::Result<Track, ModelError>(
        runtime::Ok(Track(std::make_shared<const Data>(std::move(next_data)))));
}

runtime::Result<Track, ModelError> Track::insert_clip(Clip clip) const {
    if (!clip.id().valid())
        return fail<Track>(ModelErrorCode::InvalidItemId, clip.id());
    if (find_clip(clip.id()))
        return fail<Track>(ModelErrorCode::DuplicateItemId, clip.id());
    if (const auto collision = automation_identity_collision(clip, *data_->automation_owned_ids))
        return fail<Track>(ModelErrorCode::DuplicateItemId, *collision);
    if (const auto collision = take_identity_collision(clip, *data_->take_owned_ids))
        return fail<Track>(ModelErrorCode::DuplicateItemId, *collision);
    if (const auto first = clips().empty() ? nullptr : &clips()[0]) {
        if (clip.time_anchor() != first->time_anchor())
            return fail<Track>(ModelErrorCode::MixedTimeAnchors, data_->id, clip.id());
        if (clip.time_anchor() == ClipTimeAnchor::Absolute &&
            clip.absolute_sample_rate() != first->absolute_sample_rate())
            return fail<Track>(ModelErrorCode::IncompatibleSampleRate, data_->id, clip.id());
    }
    const ItemId inserted_id = clip.id();
    auto by_start = data_->clips_by_start;
    auto by_id = data_->clips_by_id;
    bool duplicate = false;
    by_start = insert(std::move(by_start), clip, start_less, duplicate);
    duplicate = false;
    by_id = insert(std::move(by_id), std::move(clip), id_less, duplicate);
    const auto* inserted = find_id(by_id, inserted_id);
    const auto* previous = predecessor(by_start, *inserted, start_less);
    const auto* next = successor(by_start, *inserted, start_less);
    if (previous && ranges_overlap(*previous, *inserted))
        return fail<Track>(ModelErrorCode::OverlappingClips, previous->id(), inserted->id());
    if (next && ranges_overlap(*inserted, *next))
        return fail<Track>(ModelErrorCode::OverlappingClips, inserted->id(), next->id());
    auto next_data = *data_;
    next_data.clips_by_start = std::move(by_start);
    next_data.clips_by_id = std::move(by_id);
    return runtime::Result<Track, ModelError>(
        runtime::Ok(Track(std::make_shared<const Data>(std::move(next_data)))));
}

runtime::Result<Track, ModelError> Track::erase_clip(ItemId id) const {
    const auto* old = find_clip(id);
    if (!old)
        return fail<Track>(ModelErrorCode::MissingItem, id);
    auto by_start = erase(data_->clips_by_start, *old, start_less);
    auto by_id = erase(data_->clips_by_id, *old, id_less);
    auto next_data = *data_;
    next_data.clips_by_start = std::move(by_start);
    next_data.clips_by_id = std::move(by_id);
    return runtime::Result<Track, ModelError>(
        runtime::Ok(Track(std::make_shared<const Data>(std::move(next_data)))));
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
    for (const auto owned : *automation_owned_ids)
        if (std::binary_search(data_->take_owned_ids->begin(), data_->take_owned_ids->end(), owned))
            return fail<Track>(ModelErrorCode::DuplicateItemId, owned);
    auto next_data = *data_;
    next_data.automation_lanes = std::move(storage);
    next_data.automation_owned_ids = std::move(automation_owned_ids);
    return runtime::Ok(Track(std::make_shared<const Data>(std::move(next_data))));
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
    auto next_data = *data_;
    next_data.automation_lanes = std::move(storage);
    next_data.automation_owned_ids = std::move(automation_owned_ids);
    return runtime::Ok(Track(std::make_shared<const Data>(std::move(next_data))));
}

runtime::Result<Track, ModelError> Track::insert_take_lane(TakeLane lane) const {
    auto lanes = *data_->take_lanes;
    const auto found =
        std::lower_bound(lanes.begin(), lanes.end(), lane.id(),
                         [](const TakeLane& candidate, ItemId id) { return candidate.id() < id; });
    if (found != lanes.end() && found->id() == lane.id())
        return fail<Track>(ModelErrorCode::DuplicateItemId, lane.id());
    lanes.insert(found, std::move(lane));
    // Takes must be disjoint from every other track-owned id: the non-take id set
    // plus the automation-owned ids computed for this track.
    auto other_ids = non_automation_ids(data_->id, *data_->device_chain, clips());
    other_ids.insert(other_ids.end(), data_->automation_owned_ids->begin(),
                     data_->automation_owned_ids->end());
    if (const auto error = validate_attached_takes(lanes, other_ids))
        return fail<Track>(error->code, error->item, error->related_item);
    auto storage = std::make_shared<const std::vector<TakeLane>>(std::move(lanes));
    auto take_owned_ids = canonical_take_owned_ids(*storage);
    auto next_data = *data_;
    next_data.take_lanes = std::move(storage);
    next_data.take_owned_ids = std::move(take_owned_ids);
    return runtime::Ok(Track(std::make_shared<const Data>(std::move(next_data))));
}

runtime::Result<Track, ModelError> Track::erase_take_lane(ItemId id) const {
    if (data_->active_take_lane_id == id)
        return fail<Track>(ModelErrorCode::ActiveTakeLaneRemoval, id, data_->id);
    auto lanes = *data_->take_lanes;
    const auto found = std::lower_bound(
        lanes.begin(), lanes.end(), id,
        [](const TakeLane& candidate, ItemId wanted) { return candidate.id() < wanted; });
    if (found == lanes.end() || found->id() != id)
        return fail<Track>(ModelErrorCode::MissingItem, id);
    lanes.erase(found);
    auto storage = std::make_shared<const std::vector<TakeLane>>(std::move(lanes));
    auto take_owned_ids = canonical_take_owned_ids(*storage);
    auto next_data = *data_;
    next_data.take_lanes = std::move(storage);
    next_data.take_owned_ids = std::move(take_owned_ids);
    return runtime::Ok(Track(std::make_shared<const Data>(std::move(next_data))));
}

runtime::Result<Track, ModelError> Track::insert_take(ItemId lane_id, Take take) const {
    auto lanes = *data_->take_lanes;
    const auto found = std::lower_bound(
        lanes.begin(), lanes.end(), lane_id,
        [](const TakeLane& candidate, ItemId wanted) { return candidate.id() < wanted; });
    if (found == lanes.end() || found->id() != lane_id)
        return fail<Track>(ModelErrorCode::MissingItem, lane_id, data_->id);
    auto next_lane = found->insert_take(std::move(take));
    if (!next_lane)
        return fail<Track>(next_lane.error().code, next_lane.error().item,
                           next_lane.error().related_item);
    *found = std::move(next_lane).value();

    auto other_ids = non_automation_ids(data_->id, *data_->device_chain, clips());
    other_ids.insert(other_ids.end(), data_->automation_owned_ids->begin(),
                     data_->automation_owned_ids->end());
    if (const auto error = validate_attached_takes(lanes, other_ids))
        return fail<Track>(error->code, error->item, error->related_item);
    auto storage = std::make_shared<const std::vector<TakeLane>>(std::move(lanes));
    auto next_data = *data_;
    next_data.take_lanes = std::move(storage);
    next_data.take_owned_ids = canonical_take_owned_ids(*next_data.take_lanes);
    return runtime::Ok(Track(std::make_shared<const Data>(std::move(next_data))));
}

runtime::Result<Track, ModelError> Track::erase_take(ItemId lane_id, ItemId take_id) const {
    auto lanes = *data_->take_lanes;
    const auto found = std::lower_bound(
        lanes.begin(), lanes.end(), lane_id,
        [](const TakeLane& candidate, ItemId wanted) { return candidate.id() < wanted; });
    if (found == lanes.end() || found->id() != lane_id)
        return fail<Track>(ModelErrorCode::MissingItem, lane_id, data_->id);
    auto next_lane = found->erase_take(take_id);
    if (!next_lane)
        return fail<Track>(next_lane.error().code, next_lane.error().item,
                           next_lane.error().related_item);
    *found = std::move(next_lane).value();

    auto storage = std::make_shared<const std::vector<TakeLane>>(std::move(lanes));
    auto next_data = *data_;
    next_data.take_lanes = std::move(storage);
    next_data.take_owned_ids = canonical_take_owned_ids(*next_data.take_lanes);
    return runtime::Ok(Track(std::make_shared<const Data>(std::move(next_data))));
}

runtime::Result<Track, ModelError> Track::with_take_comp(ItemId lane_id,
                                                         std::vector<TakeCompSegment> comp) const {
    auto lanes = *data_->take_lanes;
    const auto found = std::lower_bound(
        lanes.begin(), lanes.end(), lane_id,
        [](const TakeLane& candidate, ItemId wanted) { return candidate.id() < wanted; });
    if (found == lanes.end() || found->id() != lane_id)
        return fail<Track>(ModelErrorCode::MissingItem, lane_id, data_->id);
    auto next_lane = found->with_comp_segments(std::move(comp));
    if (!next_lane)
        return fail<Track>(next_lane.error().code, next_lane.error().item,
                           next_lane.error().related_item);
    *found = std::move(next_lane).value();
    auto next_data = *data_;
    next_data.take_lanes = std::make_shared<const std::vector<TakeLane>>(std::move(lanes));
    return runtime::Ok(Track(std::make_shared<const Data>(std::move(next_data))));
}

Track Track::with_record_armed(bool armed) const {
    auto next_data = *data_;
    next_data.record_armed = armed;
    return Track(std::make_shared<const Data>(std::move(next_data)));
}

runtime::Result<Track, ModelError> Track::with_active_take_lane(ItemId lane_id) const {
    if (lane_id.value != 0 && !lane_id.valid())
        return fail<Track>(ModelErrorCode::InvalidItemId, lane_id, data_->id);
    if (lane_id.valid() && !find_take_lane(lane_id))
        return fail<Track>(ModelErrorCode::MissingItem, lane_id, data_->id);
    auto next_data = *data_;
    next_data.active_take_lane_id = lane_id;
    return runtime::Ok(Track(std::make_shared<const Data>(std::move(next_data))));
}

runtime::Result<Track, ModelError> Track::with_freeze(std::optional<TrackFreeze> freeze) const {
    if (freeze) {
        if (const auto error = track_freeze_error(*freeze)) {
            return fail<Track>(*error, data_->id, freeze->media.asset_id);
        }
    }
    auto next_data = *data_;
    next_data.freeze = std::move(freeze);
    return runtime::Ok(Track(std::make_shared<const Data>(std::move(next_data))));
}

ItemId Track::id() const noexcept {
    return data_->id;
}
const std::string& Track::name() const noexcept {
    return data_->name;
}
Track::ClipView Track::clips() const noexcept {
    return ClipView(data_->clips_by_start, count(data_->clips_by_start));
}
const Clip* Track::find_clip(ItemId id) const noexcept {
    if (!id.valid())
        return nullptr;
    const auto* indexed = find_id(data_->clips_by_id, id);
    return indexed ? find(data_->clips_by_start, *indexed, start_less) : nullptr;
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
std::span<const TakeLane> Track::take_lanes() const noexcept {
    return *data_->take_lanes;
}
const TakeLane* Track::find_take_lane(ItemId id) const noexcept {
    if (!id.valid())
        return nullptr;
    const auto found = std::lower_bound(
        data_->take_lanes->begin(), data_->take_lanes->end(), id,
        [](const TakeLane& candidate, ItemId wanted) { return candidate.id() < wanted; });
    return found != data_->take_lanes->end() && found->id() == id ? &*found : nullptr;
}
bool Track::record_armed() const noexcept {
    return data_->record_armed;
}
ItemId Track::active_take_lane_id() const noexcept {
    return data_->active_take_lane_id;
}
const std::optional<TrackFreeze>& Track::freeze() const noexcept {
    return data_->freeze;
}
std::size_t Track::shared_index_nodes_with(const Track& other) const {
    std::unordered_set<const ClipIndexNode*> addresses;
    collect_addresses(data_->clips_by_start, addresses);
    collect_addresses(data_->clips_by_id, addresses);
    return count_shared(other.data_->clips_by_start, addresses) +
           count_shared(other.data_->clips_by_id, addresses);
}
bool Track::shares_storage_with(const Track& other) const noexcept {
    return data_.get() == other.data_.get();
}
TrackIndexStats Track::index_stats() noexcept {
    return {g_live_index_nodes.load(), g_created_index_nodes.load()};
}

const Clip& Track::ClipView::operator[](std::size_t index) const noexcept {
    return select(root_, index);
}
const Clip& Track::ClipView::Iterator::operator*() const noexcept {
    return select(root_, index_);
}
const Clip* Track::ClipView::Iterator::operator->() const noexcept {
    return &select(root_, index_);
}
Track::ClipView::Iterator& Track::ClipView::Iterator::operator++() noexcept {
    ++index_;
    return *this;
}

} // namespace pulp::timeline
