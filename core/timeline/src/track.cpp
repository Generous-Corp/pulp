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

std::optional<ItemId> automation_identity_collision(
    const Clip& clip, std::span<const ItemId> automation_owned_ids) noexcept {
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

std::optional<std::pair<ItemId, ItemId>> first_overlap(const NodePtr& root) {
    const Clip* previous = nullptr;
    for (std::size_t i = 0; i < count(root); ++i) {
        const auto& current = select(root, i);
        if (previous && ranges_overlap(*previous, current))
            return std::pair(previous->id(), current.id());
        previous = &current;
    }
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
    NodePtr by_start;
    NodePtr by_id;
    for (auto& clip : input.clips) {
        bool duplicate_start = false;
        bool duplicate_id = false;
        by_start = insert(std::move(by_start), clip, start_less, duplicate_start);
        by_id = insert(std::move(by_id), clip, id_less, duplicate_id);
        if (duplicate_id)
            return fail<Track>(ModelErrorCode::DuplicateItemId, clip.id());
    }
    if (const auto overlap = first_overlap(by_start))
        return fail<Track>(ModelErrorCode::OverlappingClips, overlap->first, overlap->second);
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
    return runtime::Result<Track, ModelError>(runtime::Ok(Track(std::make_shared<const Data>(
        Data{input.id, std::move(input.name), std::move(by_start), std::move(by_id),
             std::move(device_chain), std::move(automation_lanes),
             std::move(automation_owned_ids)}))));
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
    return runtime::Result<Track, ModelError>(runtime::Ok(Track(std::make_shared<const Data>(
        Data{data_->id, data_->name, std::move(by_start), std::move(by_id), data_->device_chain,
             data_->automation_lanes, data_->automation_owned_ids}))));
}

runtime::Result<Track, ModelError> Track::insert_clip(Clip clip) const {
    if (!clip.id().valid())
        return fail<Track>(ModelErrorCode::InvalidItemId, clip.id());
    if (find_clip(clip.id()))
        return fail<Track>(ModelErrorCode::DuplicateItemId, clip.id());
    if (const auto collision = automation_identity_collision(clip, *data_->automation_owned_ids))
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
    return runtime::Result<Track, ModelError>(runtime::Ok(Track(std::make_shared<const Data>(
        Data{data_->id, data_->name, std::move(by_start), std::move(by_id), data_->device_chain,
             data_->automation_lanes, data_->automation_owned_ids}))));
}

runtime::Result<Track, ModelError> Track::erase_clip(ItemId id) const {
    const auto* old = find_clip(id);
    if (!old)
        return fail<Track>(ModelErrorCode::MissingItem, id);
    auto by_start = erase(data_->clips_by_start, *old, start_less);
    auto by_id = erase(data_->clips_by_id, *old, id_less);
    return runtime::Result<Track, ModelError>(runtime::Ok(Track(std::make_shared<const Data>(
        Data{data_->id, data_->name, std::move(by_start), std::move(by_id), data_->device_chain,
             data_->automation_lanes, data_->automation_owned_ids}))));
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
        Data{data_->id, data_->name, data_->clips_by_start, data_->clips_by_id, data_->device_chain,
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
        Data{data_->id, data_->name, data_->clips_by_start, data_->clips_by_id, data_->device_chain,
             std::move(storage), std::move(automation_owned_ids)})));
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
