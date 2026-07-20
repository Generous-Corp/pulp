#include <pulp/timeline/model.hpp>

#include <algorithm>
#include <atomic>
#include <exception>
#include <tuple>
#include <unordered_set>

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, ModelError> fail(ModelErrorCode code, ItemId item = {}, ItemId related = {}) {
    return runtime::Err(ModelError{code, item, related});
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

std::atomic<std::uint64_t> live_index_nodes{0};
std::atomic<std::uint64_t> created_index_nodes{0};

} // namespace

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
        ++live_index_nodes;
        ++created_index_nodes;
    }
    ~ClipIndexNode() {
        --live_index_nodes;
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

struct ClipLane::Data {
    NodePtr clips_by_start;
    NodePtr clips_by_id;
};

runtime::Result<ClipLane, ModelError> ClipLane::create(std::vector<Clip> clips) {
    if (!clips.empty()) {
        const auto& first = clips.front();
        for (const auto& clip : clips) {
            if (clip.time_anchor() != first.time_anchor())
                return fail<ClipLane>(ModelErrorCode::MixedTimeAnchors, first.id(), clip.id());
            if (first.time_anchor() == ClipTimeAnchor::Absolute &&
                clip.absolute_sample_rate() != first.absolute_sample_rate())
                return fail<ClipLane>(ModelErrorCode::IncompatibleSampleRate, first.id(),
                                      clip.id());
        }
    }
    NodePtr by_start;
    NodePtr by_id;
    for (auto& clip : clips) {
        bool duplicate_start = false;
        bool duplicate_id = false;
        by_start = insert(std::move(by_start), clip, start_less, duplicate_start);
        by_id = insert(std::move(by_id), clip, id_less, duplicate_id);
        if (duplicate_id)
            return fail<ClipLane>(ModelErrorCode::DuplicateItemId, clip.id());
    }
    if (const auto overlap = first_overlap(by_start))
        return fail<ClipLane>(ModelErrorCode::OverlappingClips, overlap->first, overlap->second);
    return runtime::Ok(
        ClipLane(std::make_shared<const Data>(Data{std::move(by_start), std::move(by_id)})));
}

runtime::Result<ClipLane, ModelError> ClipLane::replace_clip(Clip replacement) const {
    const Clip probe = replacement;
    const ItemId replacement_id = replacement.id();
    const auto* old = find(data_->clips_by_id, probe, id_less);
    if (!old)
        return fail<ClipLane>(ModelErrorCode::InvalidItemId, replacement.id());
    if (replacement.time_anchor() != old->time_anchor())
        return fail<ClipLane>(ModelErrorCode::MixedTimeAnchors, old->id(), replacement.id());
    if (replacement.time_anchor() == ClipTimeAnchor::Absolute &&
        replacement.absolute_sample_rate() != old->absolute_sample_rate())
        return fail<ClipLane>(ModelErrorCode::IncompatibleSampleRate, old->id(), replacement.id());
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
        return fail<ClipLane>(ModelErrorCode::OverlappingClips, previous->id(), inserted->id());
    if (next && ranges_overlap(*inserted, *next))
        return fail<ClipLane>(ModelErrorCode::OverlappingClips, inserted->id(), next->id());
    return runtime::Ok(
        ClipLane(std::make_shared<const Data>(Data{std::move(by_start), std::move(by_id)})));
}

runtime::Result<ClipLane, ModelError> ClipLane::insert_clip(Clip clip) const {
    if (!clip.id().valid())
        return fail<ClipLane>(ModelErrorCode::InvalidItemId, clip.id());
    if (find_clip(clip.id()))
        return fail<ClipLane>(ModelErrorCode::DuplicateItemId, clip.id());
    if (const auto first = clips().empty() ? nullptr : &clips()[0]) {
        if (clip.time_anchor() != first->time_anchor())
            return fail<ClipLane>(ModelErrorCode::MixedTimeAnchors, first->id(), clip.id());
        if (clip.time_anchor() == ClipTimeAnchor::Absolute &&
            clip.absolute_sample_rate() != first->absolute_sample_rate())
            return fail<ClipLane>(ModelErrorCode::IncompatibleSampleRate, first->id(), clip.id());
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
        return fail<ClipLane>(ModelErrorCode::OverlappingClips, previous->id(), inserted->id());
    if (next && ranges_overlap(*inserted, *next))
        return fail<ClipLane>(ModelErrorCode::OverlappingClips, inserted->id(), next->id());
    return runtime::Ok(
        ClipLane(std::make_shared<const Data>(Data{std::move(by_start), std::move(by_id)})));
}

runtime::Result<ClipLane, ModelError> ClipLane::erase_clip(ItemId id) const {
    const auto* old = find_clip(id);
    if (!old)
        return fail<ClipLane>(ModelErrorCode::MissingItem, id);
    auto by_start = erase(data_->clips_by_start, *old, start_less);
    auto by_id = erase(data_->clips_by_id, *old, id_less);
    return runtime::Ok(
        ClipLane(std::make_shared<const Data>(Data{std::move(by_start), std::move(by_id)})));
}

ClipLane::ClipView ClipLane::clips() const noexcept {
    return ClipView(data_->clips_by_start, count(data_->clips_by_start));
}

const Clip* ClipLane::find_clip(ItemId id) const noexcept {
    if (!id.valid())
        return nullptr;
    const auto* indexed = find_id(data_->clips_by_id, id);
    return indexed ? find(data_->clips_by_start, *indexed, start_less) : nullptr;
}

std::size_t ClipLane::shared_index_nodes_with(const ClipLane& other) const {
    std::unordered_set<const ClipIndexNode*> addresses;
    collect_addresses(data_->clips_by_start, addresses);
    collect_addresses(data_->clips_by_id, addresses);
    return count_shared(other.data_->clips_by_start, addresses) +
           count_shared(other.data_->clips_by_id, addresses);
}

bool ClipLane::shares_storage_with(const ClipLane& other) const noexcept {
    return data_.get() == other.data_.get();
}

ClipLaneIndexStats ClipLane::index_stats() noexcept {
    return {live_index_nodes.load(), created_index_nodes.load()};
}

const Clip& ClipLane::ClipView::operator[](std::size_t index) const noexcept {
    return select(root_, index);
}

const Clip& ClipLane::ClipView::Iterator::operator*() const noexcept {
    return select(root_, index_);
}

const Clip* ClipLane::ClipView::Iterator::operator->() const noexcept {
    return &select(root_, index_);
}

ClipLane::ClipView::Iterator& ClipLane::ClipView::Iterator::operator++() noexcept {
    ++index_;
    return *this;
}

} // namespace pulp::timeline
