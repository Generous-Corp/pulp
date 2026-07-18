#include <pulp/timeline/model.hpp>

#include "identity_directory.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <tuple>
#include <unordered_set>

namespace pulp::timeline {

struct IdRemapBuilder {
    static std::vector<std::pair<ItemId, ItemId>>& entries(IdRemapTable& table) noexcept {
        return table.entries_;
    }
};

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

} // namespace

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

struct Clip::Data {
    ItemId id;
    ClipTimeRange range;
    ClipContent content;
};

runtime::Result<Clip, ModelError> Clip::create(ItemId id, timebase::TickPosition start,
                                               timebase::TickDuration duration,
                                               ClipContent content) {
    if (!id.valid())
        return fail<Clip>(ModelErrorCode::InvalidItemId, id);
    if (!positive_range(start.value, duration.value))
        return fail<Clip>(ModelErrorCode::InvalidDuration, id);
    if (const auto* media = std::get_if<MediaRef>(&content)) {
        if (!media->asset_id.valid() || media->source_start.value < 0 || media->frame_count == 0 ||
            static_cast<std::uint64_t>(media->source_start.value) >
                std::numeric_limits<std::uint64_t>::max() - media->frame_count)
            return fail<Clip>(ModelErrorCode::InvalidMediaRange, id, media->asset_id);
    }
    return runtime::Result<Clip, ModelError>(runtime::Ok(Clip(std::make_shared<const Data>(
        Data{id, MusicalTimeRange{start, duration}, std::move(content)}))));
}

runtime::Result<Clip, ModelError> Clip::create_absolute(ItemId id, timebase::SamplePosition start,
                                                        std::uint64_t sample_count,
                                                        timebase::RationalRate sample_rate,
                                                        ClipContent content) {
    if (!id.valid())
        return fail<Clip>(ModelErrorCode::InvalidItemId, id);
    if (!sample_rate.valid())
        return fail<Clip>(ModelErrorCode::InvalidSampleRate, id);
    if (sample_count == 0 ||
        sample_count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        start.value >
            std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(sample_count))
        return fail<Clip>(ModelErrorCode::InvalidDuration, id);
    if (const auto* media = std::get_if<MediaRef>(&content)) {
        if (!media->asset_id.valid() || media->source_start.value < 0 || media->frame_count == 0 ||
            static_cast<std::uint64_t>(media->source_start.value) >
                std::numeric_limits<std::uint64_t>::max() - media->frame_count)
            return fail<Clip>(ModelErrorCode::InvalidMediaRange, id, media->asset_id);
    }
    return runtime::Result<Clip, ModelError>(runtime::Ok(Clip(std::make_shared<const Data>(
        Data{id, AbsoluteTimeRange{start, sample_count, sample_rate.normalized()},
             std::move(content)}))));
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

runtime::Result<Clip, ModelError> Clip::with_time_range(ClipTimeRange range) const {
    if (const auto* musical = std::get_if<MusicalTimeRange>(&range))
        return create(id(), musical->start, musical->duration, content());
    const auto& absolute = std::get<AbsoluteTimeRange>(range);
    return create_absolute(id(), absolute.start, absolute.sample_count, absolute.sample_rate,
                           content());
}

runtime::Result<Clip, ModelError> Clip::with_content(ClipContent replacement) const {
    if (time_anchor() == ClipTimeAnchor::Musical)
        return create(id(), start(), duration(), std::move(replacement));
    return create_absolute(id(), absolute_start(), absolute_duration_samples(),
                           absolute_sample_rate(), std::move(replacement));
}

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
};

runtime::Result<Track, ModelError> Track::create(ItemId id, std::string name,
                                                 std::vector<Clip> clips) {
    if (!id.valid())
        return fail<Track>(ModelErrorCode::InvalidItemId, id);
    if (!clips.empty()) {
        const auto anchor = clips.front().time_anchor();
        const auto absolute_rate = clips.front().absolute_sample_rate();
        for (const auto& clip : clips) {
            if (clip.time_anchor() != anchor)
                return fail<Track>(ModelErrorCode::MixedTimeAnchors, id, clip.id());
            if (anchor == ClipTimeAnchor::Absolute && clip.absolute_sample_rate() != absolute_rate)
                return fail<Track>(ModelErrorCode::IncompatibleSampleRate, id, clip.id());
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
            return fail<Track>(ModelErrorCode::DuplicateItemId, clip.id());
    }
    if (const auto overlap = first_overlap(by_start))
        return fail<Track>(ModelErrorCode::OverlappingClips, overlap->first, overlap->second);
    return runtime::Result<Track, ModelError>(runtime::Ok(Track(std::make_shared<const Data>(
        Data{id, std::move(name), std::move(by_start), std::move(by_id)}))));
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
        Data{data_->id, data_->name, std::move(by_start), std::move(by_id)}))));
}

runtime::Result<Track, ModelError> Track::insert_clip(Clip clip) const {
    if (!clip.id().valid())
        return fail<Track>(ModelErrorCode::InvalidItemId, clip.id());
    if (find_clip(clip.id()))
        return fail<Track>(ModelErrorCode::DuplicateItemId, clip.id());
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
        Data{data_->id, data_->name, std::move(by_start), std::move(by_id)}))));
}

runtime::Result<Track, ModelError> Track::erase_clip(ItemId id) const {
    const auto* old = find_clip(id);
    if (!old)
        return fail<Track>(ModelErrorCode::MissingItem, id);
    auto by_start = erase(data_->clips_by_start, *old, start_less);
    auto by_id = erase(data_->clips_by_id, *old, id_less);
    return runtime::Result<Track, ModelError>(runtime::Ok(Track(std::make_shared<const Data>(
        Data{data_->id, data_->name, std::move(by_start), std::move(by_id)}))));
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
    detail::IdentityDirectory identities;
};

runtime::Result<Project, ModelError> Project::create(ProjectInput input) {
    if (!input.id.valid())
        return fail<Project>(ModelErrorCode::InvalidItemId, input.id);
    std::vector<ItemId> all_ids{input.id};
    std::uint64_t maximum_id = input.id.value;
    for (const auto& asset : input.assets) {
        if (!asset.id.valid())
            return fail<Project>(ModelErrorCode::InvalidItemId, asset.id);
        if (!asset.sample_rate.valid())
            return fail<Project>(ModelErrorCode::InvalidSampleRate, asset.id);
        all_ids.push_back(asset.id);
        maximum_id = std::max(maximum_id, asset.id.value);
    }
    for (const auto& sequence : input.sequences) {
        all_ids.push_back(sequence.id());
        maximum_id = std::max(maximum_id, sequence.id().value);
        for (const auto& track : sequence.tracks()) {
            all_ids.push_back(track.id());
            maximum_id = std::max(maximum_id, track.id().value);
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
    for (const auto& sequence : input.sequences) {
        for (const auto& track : sequence.tracks()) {
            for (const auto& clip : track.clips()) {
                if (const auto* media = std::get_if<MediaRef>(&clip.content())) {
                    const auto found = std::lower_bound(
                        input.assets.begin(), input.assets.end(), media->asset_id,
                        [](const MediaAsset& asset, ItemId id) { return asset.id < id; });
                    if (found == input.assets.end() || found->id != media->asset_id)
                        return fail<Project>(ModelErrorCode::MissingAsset, clip.id(),
                                             media->asset_id);
                    const auto source_start = static_cast<std::uint64_t>(media->source_start.value);
                    if (source_start > found->frame_count ||
                        media->frame_count > found->frame_count - source_start)
                        return fail<Project>(ModelErrorCode::InvalidMediaRange, clip.id(),
                                             media->asset_id);
                }
            }
        }
    }
    detail::IdentityDirectory identities;
    auto add_identity = [&](ItemId id, ItemLocation location) { identities.insert(id, location); };
    add_identity(input.id, {ItemKind::Project, {}, {}, {}, true});
    for (const auto& asset : input.assets)
        add_identity(asset.id, {ItemKind::Asset, {}, {}, {}, true});
    for (const auto& sequence : input.sequences) {
        add_identity(sequence.id(), {ItemKind::Sequence, sequence.id(), {}, {}, true});
        for (const auto& track : sequence.tracks()) {
            add_identity(track.id(), {ItemKind::Track, sequence.id(), track.id(), {}, true});
            for (const auto& clip : track.clips()) {
                add_identity(clip.id(),
                             {ItemKind::Clip, sequence.id(), track.id(), clip.id(), true});
                if (const auto* notes = std::get_if<NoteContent>(&clip.content())) {
                    for (const auto& note : notes->notes())
                        add_identity(note.id,
                                     {ItemKind::Note, sequence.id(), track.id(), clip.id(), true});
                }
            }
        }
    }
    return runtime::Result<Project, ModelError>(runtime::Ok(Project(std::make_shared<const Data>(
        Data{input.id, std::move(input.name), input.next_item_id, input.root_sequence_id,
             std::move(input.assets), std::move(input.sequences), std::move(identities)}))));
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
    for (const auto& change : mutations) {
        if (!change.item.valid())
            return fail<Project>(ModelErrorCode::InvalidItemId, change.item);
        const auto existing = identities.locate(change.item);
        switch (change.mutation) {
        case IdentityMutationKind::Insert: {
            if (existing)
                return fail<Project>(ModelErrorCode::IdentityConflict, change.item);
            auto location = change.location;
            location.active = true;
            identities.insert(change.item, location);
            break;
        }
        case IdentityMutationKind::Deactivate: {
            if (!existing || !existing->active || existing->kind != change.location.kind ||
                existing->sequence_id != change.location.sequence_id ||
                existing->track_id != change.location.track_id ||
                existing->clip_id != change.location.clip_id)
                return fail<Project>(ModelErrorCode::InvalidIdentityTransition, change.item);
            auto location = *existing;
            location.active = false;
            identities.replace(change.item, location);
            break;
        }
        case IdentityMutationKind::Reactivate: {
            if (!existing || existing->active || existing->kind != change.location.kind ||
                existing->sequence_id != change.location.sequence_id ||
                existing->track_id != change.location.track_id ||
                existing->clip_id != change.location.clip_id)
                return fail<Project>(ModelErrorCode::InvalidIdentityTransition, change.item);
            auto location = *existing;
            location.active = true;
            identities.replace(change.item, location);
            break;
        }
        }
    }
    const auto next = requested_next.value_or(data_->next_item_id);
    if (next < data_->next_item_id || next == 0)
        return fail<Project>(ModelErrorCode::NextItemIdNotMonotonic, {next}, {data_->next_item_id});
    auto sequences = data_->sequences;
    sequences[static_cast<std::size_t>(found - data_->sequences.begin())] = std::move(sequence);
    return runtime::Result<Project, ModelError>(runtime::Ok(Project(std::make_shared<const Data>(
        Data{data_->id, data_->name, next, data_->root_sequence_id, data_->assets,
             std::move(sequences), std::move(identities)}))));
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

std::optional<ItemId> IdRemapTable::find(ItemId old_id) const noexcept {
    const auto found =
        std::lower_bound(entries_.begin(), entries_.end(), old_id,
                         [](const auto& entry, ItemId wanted) { return entry.first < wanted; });
    return found != entries_.end() && found->first == old_id ? std::optional<ItemId>(found->second)
                                                             : std::nullopt;
}

runtime::Result<ItemId, ModelError> ExternalIdFixup::apply(ItemId id) const noexcept {
    return map ? map(context, id) : runtime::Result<ItemId, ModelError>(runtime::Ok(id));
}

namespace {

std::optional<ModelError> allocate_owned(IdRemapTable& table, ItemIdAllocator& allocator,
                                         ItemId old_id) {
    auto next = allocator.allocate();
    if (!next)
        return next.error();
    IdRemapBuilder::entries(table).emplace_back(old_id, next.value());
    return std::nullopt;
}

std::optional<ModelError> finish_table(IdRemapTable& table) {
    auto& entries = IdRemapBuilder::entries(table);
    std::sort(entries.begin(), entries.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    const auto duplicate =
        std::adjacent_find(entries.begin(), entries.end(),
                           [](const auto& lhs, const auto& rhs) { return lhs.first == rhs.first; });
    if (duplicate != entries.end())
        return ModelError{ModelErrorCode::DuplicateItemId, duplicate->first, {}};
    return std::nullopt;
}

std::optional<ModelError> validate_owned_ids(std::vector<ItemId> ids) {
    for (const auto id : ids)
        if (!id.valid())
            return ModelError{ModelErrorCode::InvalidItemId, id, {}};
    std::sort(ids.begin(), ids.end());
    const auto duplicate = std::adjacent_find(ids.begin(), ids.end());
    if (duplicate != ids.end())
        return ModelError{ModelErrorCode::DuplicateItemId, *duplicate, {}};
    return std::nullopt;
}

void append_clip_ids(const Clip& clip, std::vector<ItemId>& ids) {
    ids.push_back(clip.id());
    if (const auto* notes = std::get_if<NoteContent>(&clip.content()))
        for (const auto& note : notes->notes())
            ids.push_back(note.id);
}

std::optional<ModelError> preflight(const Clip& clip) {
    std::vector<ItemId> ids;
    append_clip_ids(clip, ids);
    return validate_owned_ids(std::move(ids));
}

std::optional<ModelError> preflight(const Track& track) {
    std::vector<ItemId> ids{track.id()};
    for (const auto& clip : track.clips())
        append_clip_ids(clip, ids);
    return validate_owned_ids(std::move(ids));
}

std::optional<ModelError> preflight(const Sequence& sequence) {
    std::vector<ItemId> ids{sequence.id()};
    for (const auto& track : sequence.tracks()) {
        ids.push_back(track.id());
        for (const auto& clip : track.clips())
            append_clip_ids(clip, ids);
    }
    return validate_owned_ids(std::move(ids));
}

runtime::Result<Clip, ModelError> rebuild_clip(const Clip& clip, const IdRemapTable& table,
                                               ExternalIdFixup external) {
    ClipContent content = clip.content();
    if (auto* media = std::get_if<MediaRef>(&content)) {
        auto fixed = external.apply(media->asset_id);
        if (!fixed)
            return fail<Clip>(fixed.error().code, fixed.error().item, fixed.error().related_item);
        media->asset_id = fixed.value();
    }
    if (const auto* old_notes = std::get_if<NoteContent>(&clip.content())) {
        std::vector<NoteEvent> notes(old_notes->notes().begin(), old_notes->notes().end());
        for (auto& note : notes)
            note.id = *table.find(note.id);
        auto rebuilt = NoteContent::create(std::move(notes));
        if (!rebuilt)
            return fail<Clip>(rebuilt.error().code, rebuilt.error().item,
                              rebuilt.error().related_item);
        content = std::move(rebuilt).value();
    }
    if (clip.time_anchor() == ClipTimeAnchor::Musical)
        return Clip::create(*table.find(clip.id()), clip.start(), clip.duration(),
                            std::move(content));
    return Clip::create_absolute(*table.find(clip.id()), clip.absolute_start(),
                                 clip.absolute_duration_samples(), clip.absolute_sample_rate(),
                                 std::move(content));
}

void allocate_clip_owned(const Clip& clip, IdRemapTable& table, ItemIdAllocator& allocator,
                         std::optional<ModelError>& error) {
    if (error)
        return;
    error = allocate_owned(table, allocator, clip.id());
    if (const auto* notes = std::get_if<NoteContent>(&clip.content()))
        for (const auto& note : notes->notes()) {
            if (error)
                return;
            error = allocate_owned(table, allocator, note.id);
        }
}

runtime::Result<Track, ModelError> rebuild_track(const Track& track, const IdRemapTable& table,
                                                 ExternalIdFixup external) {
    std::vector<Clip> clips;
    clips.reserve(track.clips().size());
    for (const auto& clip : track.clips()) {
        auto rebuilt = rebuild_clip(clip, table, external);
        if (!rebuilt)
            return fail<Track>(rebuilt.error().code, rebuilt.error().item,
                               rebuilt.error().related_item);
        clips.push_back(std::move(rebuilt).value());
    }
    return Track::create(*table.find(track.id()), track.name(), std::move(clips));
}

runtime::Result<Sequence, ModelError>
rebuild_sequence(const Sequence& sequence, const IdRemapTable& table, ExternalIdFixup external) {
    std::vector<Track> tracks;
    tracks.reserve(sequence.tracks().size());
    for (const auto& track : sequence.tracks()) {
        auto rebuilt = rebuild_track(track, table, external);
        if (!rebuilt)
            return fail<Sequence>(rebuilt.error().code, rebuilt.error().item,
                                  rebuilt.error().related_item);
        tracks.push_back(std::move(rebuilt).value());
    }
    return Sequence::create(*table.find(sequence.id()), sequence.name(), sequence.duration(),
                            sequence.absolute_duration(), std::move(tracks));
}

} // namespace

runtime::Result<RemappedClip, ModelError> remap_ids(const Clip& clip, ItemIdAllocator& allocator,
                                                    ExternalIdFixup external) {
    if (const auto error = preflight(clip))
        return fail<RemappedClip>(error->code, error->item, error->related_item);
    auto working = allocator;
    IdRemapTable table;
    std::optional<ModelError> error;
    allocate_clip_owned(clip, table, working, error);
    if (error)
        return fail<RemappedClip>(error->code, error->item, error->related_item);
    if (const auto table_error = finish_table(table))
        return fail<RemappedClip>(table_error->code, table_error->item, table_error->related_item);
    auto rebuilt = rebuild_clip(clip, table, external);
    if (!rebuilt)
        return fail<RemappedClip>(rebuilt.error().code, rebuilt.error().item,
                                  rebuilt.error().related_item);
    allocator = working;
    return runtime::Result<RemappedClip, ModelError>(
        runtime::Ok(RemappedClip{std::move(rebuilt).value(), std::move(table)}));
}

runtime::Result<RemappedTrack, ModelError> remap_ids(const Track& track, ItemIdAllocator& allocator,
                                                     ExternalIdFixup external) {
    if (const auto error = preflight(track))
        return fail<RemappedTrack>(error->code, error->item, error->related_item);
    auto working = allocator;
    IdRemapTable table;
    std::optional<ModelError> error = allocate_owned(table, working, track.id());
    for (const auto& clip : track.clips())
        allocate_clip_owned(clip, table, working, error);
    if (error)
        return fail<RemappedTrack>(error->code, error->item, error->related_item);
    if (const auto table_error = finish_table(table))
        return fail<RemappedTrack>(table_error->code, table_error->item, table_error->related_item);
    auto rebuilt = rebuild_track(track, table, external);
    if (!rebuilt)
        return fail<RemappedTrack>(rebuilt.error().code, rebuilt.error().item,
                                   rebuilt.error().related_item);
    allocator = working;
    return runtime::Result<RemappedTrack, ModelError>(
        runtime::Ok(RemappedTrack{std::move(rebuilt).value(), std::move(table)}));
}

runtime::Result<RemappedSequence, ModelError>
remap_ids(const Sequence& sequence, ItemIdAllocator& allocator, ExternalIdFixup external) {
    if (const auto error = preflight(sequence))
        return fail<RemappedSequence>(error->code, error->item, error->related_item);
    auto working = allocator;
    IdRemapTable table;
    std::optional<ModelError> error = allocate_owned(table, working, sequence.id());
    for (const auto& track : sequence.tracks()) {
        if (!error)
            error = allocate_owned(table, working, track.id());
        for (const auto& clip : track.clips())
            allocate_clip_owned(clip, table, working, error);
    }
    if (error)
        return fail<RemappedSequence>(error->code, error->item, error->related_item);
    if (const auto table_error = finish_table(table))
        return fail<RemappedSequence>(table_error->code, table_error->item,
                                      table_error->related_item);
    auto rebuilt = rebuild_sequence(sequence, table, external);
    if (!rebuilt)
        return fail<RemappedSequence>(rebuilt.error().code, rebuilt.error().item,
                                      rebuilt.error().related_item);
    allocator = working;
    return runtime::Result<RemappedSequence, ModelError>(
        runtime::Ok(RemappedSequence{std::move(rebuilt).value(), std::move(table)}));
}

runtime::Result<RemappedProject, ModelError> remap_ids(const Project& project,
                                                       std::uint64_t first_id) {
    ItemIdAllocator allocator(first_id);
    IdRemapTable table;
    std::optional<ModelError> error = allocate_owned(table, allocator, project.id());
    for (const auto& asset : project.assets())
        if (!error)
            error = allocate_owned(table, allocator, asset.id);
    for (const auto& sequence : project.sequences()) {
        if (!error)
            error = allocate_owned(table, allocator, sequence.id());
        for (const auto& track : sequence.tracks()) {
            if (!error)
                error = allocate_owned(table, allocator, track.id());
            for (const auto& clip : track.clips())
                allocate_clip_owned(clip, table, allocator, error);
        }
    }
    if (error)
        return fail<RemappedProject>(error->code, error->item, error->related_item);
    if (const auto table_error = finish_table(table))
        return fail<RemappedProject>(table_error->code, table_error->item,
                                     table_error->related_item);

    std::vector<MediaAsset> assets;
    assets.reserve(project.assets().size());
    for (const auto& asset : project.assets()) {
        auto copy = asset;
        copy.id = *table.find(asset.id);
        assets.push_back(std::move(copy));
    }
    struct Context {
        const IdRemapTable* table;
    } context{&table};
    const ExternalIdFixup internal{
        &context, [](void* raw, ItemId id) noexcept -> runtime::Result<ItemId, ModelError> {
            const auto* ctx = static_cast<Context*>(raw);
            const auto mapped = ctx->table->find(id);
            return mapped ? runtime::Result<ItemId, ModelError>(runtime::Ok(*mapped))
                          : fail<ItemId>(ModelErrorCode::MissingAsset, {}, id);
        }};
    std::vector<Sequence> sequences;
    sequences.reserve(project.sequences().size());
    for (const auto& sequence : project.sequences()) {
        auto rebuilt = rebuild_sequence(sequence, table, internal);
        if (!rebuilt)
            return fail<RemappedProject>(rebuilt.error().code, rebuilt.error().item,
                                         rebuilt.error().related_item);
        sequences.push_back(std::move(rebuilt).value());
    }
    auto rebuilt = Project::create(ProjectInput{
        *table.find(project.id()), project.name(), allocator.next_value(),
        *table.find(project.root_sequence_id()), std::move(assets), std::move(sequences)});
    if (!rebuilt)
        return fail<RemappedProject>(rebuilt.error().code, rebuilt.error().item,
                                     rebuilt.error().related_item);
    return runtime::Result<RemappedProject, ModelError>(
        runtime::Ok(RemappedProject{std::move(rebuilt).value(), std::move(table)}));
}

} // namespace pulp::timeline
