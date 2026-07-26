#include <pulp/timeline/model.hpp>

#include "sequence_scene_internal.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, ModelError> fail(ModelErrorCode code, ItemId item = {}, ItemId related = {}) {
    return runtime::Result<T, ModelError>(runtime::Err(ModelError{code, item, related}));
}

std::atomic<std::uint64_t> g_live_launcher_nodes{0};
std::atomic<std::uint64_t> g_created_launcher_nodes{0};

template <typename Record> struct PersistentNode {
    using Ptr = std::shared_ptr<const PersistentNode>;

    Record record;
    Ptr left;
    Ptr right;
    int height = 1;
    std::size_t count = 1;

    PersistentNode(Record value, Ptr left_child = {}, Ptr right_child = {})
        : record(std::move(value)), left(std::move(left_child)), right(std::move(right_child)),
          height(1 + std::max(left ? left->height : 0, right ? right->height : 0)),
          count(1 + (left ? left->count : 0) + (right ? right->count : 0)) {
        ++g_live_launcher_nodes;
        ++g_created_launcher_nodes;
    }
    ~PersistentNode() {
        --g_live_launcher_nodes;
    }
};

template <typename Record> using NodePtr = typename PersistentNode<Record>::Ptr;

template <typename Record> ItemId record_id(const Record& record) noexcept {
    return record.id;
}

template <typename Record>
NodePtr<Record> node(Record record, NodePtr<Record> left = {}, NodePtr<Record> right = {}) {
    return std::make_shared<const PersistentNode<Record>>(std::move(record), std::move(left),
                                                          std::move(right));
}

template <typename Record>
int height(const std::shared_ptr<const PersistentNode<Record>>& value) noexcept {
    return value ? value->height : 0;
}

template <typename Record>
NodePtr<Record> rotate_left(const std::shared_ptr<const PersistentNode<Record>>& root) {
    const auto pivot = root->right;
    return node(pivot->record, node(root->record, root->left, pivot->left), pivot->right);
}

template <typename Record>
NodePtr<Record> rotate_right(const std::shared_ptr<const PersistentNode<Record>>& root) {
    const auto pivot = root->left;
    return node(pivot->record, pivot->left, node(root->record, pivot->right, root->right));
}

template <typename Record>
NodePtr<Record> rebalance(std::shared_ptr<const PersistentNode<Record>> root) {
    const int balance = height(root->left) - height(root->right);
    if (balance > 1) {
        if (height(root->left->right) > height(root->left->left))
            root = node(root->record, rotate_left(root->left), root->right);
        return rotate_right(root);
    }
    if (balance < -1) {
        if (height(root->right->left) > height(root->right->right))
            root = node(root->record, root->left, rotate_right(root->right));
        return rotate_left(root);
    }
    return root;
}

template <typename Record>
NodePtr<Record> put(std::shared_ptr<const PersistentNode<Record>> root, Record record,
                    bool& inserted) {
    if (!root) {
        inserted = true;
        return node(std::move(record));
    }
    const auto id = record_id(record);
    if (id < record_id(root->record))
        return rebalance(
            node(root->record, put(root->left, std::move(record), inserted), root->right));
    if (record_id(root->record) < id)
        return rebalance(
            node(root->record, root->left, put(root->right, std::move(record), inserted)));
    return node(std::move(record), root->left, root->right);
}

template <typename Record>
const Record* find_record(const std::shared_ptr<const PersistentNode<Record>>& root,
                          ItemId id) noexcept {
    auto current = root;
    while (current) {
        if (id < record_id(current->record))
            current = current->left;
        else if (record_id(current->record) < id)
            current = current->right;
        else
            return &current->record;
    }
    return nullptr;
}

template <typename Record>
const PersistentNode<Record>*
minimum(const std::shared_ptr<const PersistentNode<Record>>& root) noexcept {
    auto* current = root.get();
    while (current && current->left)
        current = current->left.get();
    return current;
}

template <typename Record>
NodePtr<Record> erase_record(std::shared_ptr<const PersistentNode<Record>> root, ItemId id,
                             bool& erased) {
    if (!root)
        return {};
    if (id < record_id(root->record))
        return rebalance(node(root->record, erase_record(root->left, id, erased), root->right));
    if (record_id(root->record) < id)
        return rebalance(node(root->record, root->left, erase_record(root->right, id, erased)));
    erased = true;
    if (!root->left)
        return root->right;
    if (!root->right)
        return root->left;
    const auto* successor = minimum(root->right);
    bool removed = false;
    return rebalance(node(successor->record, root->left,
                          erase_record(root->right, record_id(successor->record), removed)));
}

template <typename Record>
NodePtr<Record> build_balanced(std::span<const Record> records, std::size_t begin,
                               std::size_t end) {
    if (begin == end)
        return {};
    const auto middle = begin + (end - begin) / 2;
    return node(records[middle], build_balanced(records, begin, middle),
                build_balanced(records, middle + 1, end));
}

template <typename Record>
void collect_addresses(const std::shared_ptr<const PersistentNode<Record>>& root,
                       std::unordered_set<const void*>& addresses) {
    if (!root)
        return;
    addresses.insert(root.get());
    collect_addresses(root->left, addresses);
    collect_addresses(root->right, addresses);
}

template <typename Record>
std::size_t count_shared(const std::shared_ptr<const PersistentNode<Record>>& root,
                         const std::unordered_set<const void*>& addresses) {
    if (!root)
        return 0;
    if (addresses.contains(root.get()))
        return root->count;
    return count_shared(root->left, addresses) + count_shared(root->right, addresses);
}

struct SlotRecord {
    ItemId id;
    Slot slot;
    ItemId previous;
    ItemId next;
};

struct SceneRecord {
    ItemId id;
    Scene scene;
    ItemId previous;
    ItemId next;
};

struct IdRecord {
    ItemId id;
    ItemId value;
};

struct SetRecord {
    ItemId id;
};

struct ReferenceRecord {
    ItemId id;
    NodePtr<SetRecord> sources;
};

NodePtr<SetRecord> set_insert(NodePtr<SetRecord> root, ItemId id) {
    bool inserted = false;
    return put(std::move(root), SetRecord{id}, inserted);
}

NodePtr<SetRecord> set_erase(NodePtr<SetRecord> root, ItemId id) {
    bool erased = false;
    return erase_record(std::move(root), id, erased);
}

NodePtr<ReferenceRecord> reference_add(NodePtr<ReferenceRecord> root, ItemId target,
                                       ItemId source) {
    auto sources =
        find_record(root, target) ? find_record(root, target)->sources : NodePtr<SetRecord>{};
    bool inserted = false;
    return put(std::move(root), ReferenceRecord{target, set_insert(std::move(sources), source)},
               inserted);
}

NodePtr<ReferenceRecord> reference_remove(NodePtr<ReferenceRecord> root, ItemId target,
                                          ItemId source) {
    const auto* existing = find_record(root, target);
    if (!existing)
        return root;
    auto sources = set_erase(existing->sources, source);
    if (!sources) {
        bool erased = false;
        return erase_record(std::move(root), target, erased);
    }
    bool inserted = false;
    return put(std::move(root), ReferenceRecord{target, std::move(sources)}, inserted);
}

bool valid_follow_action_kind(FollowActionKind kind) noexcept {
    switch (kind) {
    case FollowActionKind::None:
    case FollowActionKind::Stop:
    case FollowActionKind::Again:
    case FollowActionKind::Previous:
    case FollowActionKind::Next:
    case FollowActionKind::First:
    case FollowActionKind::Last:
    case FollowActionKind::Any:
    case FollowActionKind::Other:
    case FollowActionKind::Jump:
        return true;
    }
    return false;
}

std::optional<ModelError> validate_slot_shape(const Slot& slot, ItemId scene) {
    if (!slot.id.valid())
        return ModelError{ModelErrorCode::InvalidItemId, slot.id, scene};
    if (slot.clip_id.value != 0 && !slot.clip_id.valid())
        return ModelError{ModelErrorCode::InvalidItemId, slot.clip_id, slot.id};
    if (slot.follow.choice_count > FollowActionSet::kMaxChoices)
        return ModelError{ModelErrorCode::InvalidSchemaIdentity, slot.id, scene};
    for (std::size_t index = slot.follow.choice_count; index < FollowActionSet::kMaxChoices;
         ++index)
        if (slot.follow.choices[index] != FollowAction{})
            return ModelError{ModelErrorCode::InvalidSchemaIdentity, slot.id, scene};
    for (const auto& action : slot.follow.active()) {
        if (!valid_follow_action_kind(action.kind))
            return ModelError{ModelErrorCode::InvalidSchemaIdentity, slot.id, scene};
        if (action.target.value != 0 && !action.target.valid())
            return ModelError{ModelErrorCode::InvalidItemId, action.target, slot.id};
        if (action.kind == FollowActionKind::Jump) {
            if (!action.target.valid())
                return ModelError{ModelErrorCode::MissingItem, action.target, slot.id};
        } else if (action.target.value != 0) {
            return ModelError{ModelErrorCode::InvalidSchemaIdentity, slot.id, action.target};
        }
    }
    return std::nullopt;
}

bool tracks_contain_clip(std::span<const Track> tracks, ItemId id) noexcept {
    for (const auto& track : tracks)
        if (track.find_clip(id))
            return true;
    return false;
}

} // namespace

namespace detail {

class SlotListStore {
  public:
    NodePtr<SlotRecord> slots;
    ItemId head;
    ItemId tail;
    std::size_t size = 0;
};

class SlotListAccess {
  public:
    static runtime::Result<SlotList, ModelError> make_persistent(const SlotList& source,
                                                                 ItemId scene) {
        std::vector<SlotRecord> records;
        records.reserve(source.size());
        std::vector<ItemId> ids;
        ids.reserve(source.size());
        ItemId previous;
        for (const auto& slot : source) {
            if (const auto invalid = validate_slot_shape(slot, scene))
                return runtime::Err(*invalid);
            ids.push_back(slot.id);
            records.push_back({slot.id, slot, previous, {}});
            if (records.size() > 1)
                records[records.size() - 2].next = slot.id;
            previous = slot.id;
        }
        std::sort(ids.begin(), ids.end());
        if (const auto duplicate = std::adjacent_find(ids.begin(), ids.end());
            duplicate != ids.end())
            return fail<SlotList>(ModelErrorCode::DuplicateItemId, *duplicate, scene);
        auto by_id = records;
        std::sort(by_id.begin(), by_id.end(),
                  [](const SlotRecord& lhs, const SlotRecord& rhs) { return lhs.id < rhs.id; });
        auto store = std::make_shared<SlotListStore>();
        store->slots = build_balanced<SlotRecord>(by_id, 0, by_id.size());
        store->size = records.size();
        if (!records.empty()) {
            store->head = records.front().id;
            store->tail = records.back().id;
        }
        return runtime::Ok(SlotList(std::shared_ptr<const SlotListStore>(std::move(store))));
    }

    static runtime::Result<SlotList, ModelError>
    insert(const SlotList& source, Slot slot, std::optional<ItemId> before, ItemId scene) {
        if (const auto invalid = validate_slot_shape(slot, scene))
            return runtime::Err(*invalid);
        if (source.find(slot.id))
            return fail<SlotList>(ModelErrorCode::DuplicateItemId, slot.id, scene);
        const auto store = source.store_;
        if (!store)
            return fail<SlotList>(ModelErrorCode::InvalidSchemaIdentity, scene);
        const SlotRecord* next = before ? find_record(store->slots, *before) : nullptr;
        if (before && !next)
            return fail<SlotList>(ModelErrorCode::MissingItem, *before, scene);
        const ItemId previous = next ? next->previous : store->tail;
        auto updated = std::make_shared<SlotListStore>(*store);
        if (previous.valid()) {
            auto record = *find_record(updated->slots, previous);
            record.next = slot.id;
            bool inserted = false;
            updated->slots = put(std::move(updated->slots), std::move(record), inserted);
        } else {
            updated->head = slot.id;
        }
        if (next) {
            auto record = *next;
            record.previous = slot.id;
            bool inserted = false;
            updated->slots = put(std::move(updated->slots), std::move(record), inserted);
        } else {
            updated->tail = slot.id;
        }
        bool inserted = false;
        updated->slots = put(
            std::move(updated->slots),
            SlotRecord{slot.id, std::move(slot), previous, next ? next->id : ItemId{}}, inserted);
        ++updated->size;
        return runtime::Ok(SlotList(std::shared_ptr<const SlotListStore>(std::move(updated))));
    }

    static runtime::Result<SlotList, ModelError> erase(const SlotList& source, ItemId id,
                                                       ItemId scene) {
        const auto store = source.store_;
        const auto* removed = store ? find_record(store->slots, id) : nullptr;
        if (!removed)
            return fail<SlotList>(ModelErrorCode::MissingItem, id, scene);
        auto updated = std::make_shared<SlotListStore>(*store);
        if (removed->previous.valid()) {
            auto previous = *find_record(updated->slots, removed->previous);
            previous.next = removed->next;
            bool inserted = false;
            updated->slots = put(std::move(updated->slots), std::move(previous), inserted);
        } else {
            updated->head = removed->next;
        }
        if (removed->next.valid()) {
            auto next = *find_record(updated->slots, removed->next);
            next.previous = removed->previous;
            bool inserted = false;
            updated->slots = put(std::move(updated->slots), std::move(next), inserted);
        } else {
            updated->tail = removed->previous;
        }
        bool erased = false;
        updated->slots = erase_record(std::move(updated->slots), id, erased);
        --updated->size;
        return runtime::Ok(SlotList(std::shared_ptr<const SlotListStore>(std::move(updated))));
    }

    static const SlotListStore* store(const SlotList& list) noexcept {
        return list.store_.get();
    }
};

} // namespace detail

SlotList::SlotList() : raw_(std::make_shared<const std::vector<Slot>>()) {}

SlotList::SlotList(std::vector<Slot> slots)
    : raw_(std::make_shared<const std::vector<Slot>>(std::move(slots))) {}

SlotList::SlotList(std::initializer_list<Slot> slots) : SlotList(std::vector<Slot>(slots)) {}

std::size_t SlotList::size() const noexcept {
    return raw_ ? raw_->size() : store_->size;
}

const Slot& SlotList::operator[](std::size_t index) const noexcept {
    if (raw_)
        return (*raw_)[index];
    auto current = store_->head;
    while (index--)
        current = find_record(store_->slots, current)->next;
    return find_record(store_->slots, current)->slot;
}

const Slot* SlotList::find(ItemId id) const noexcept {
    if (raw_) {
        const auto found = std::find_if(raw_->begin(), raw_->end(),
                                        [id](const Slot& slot) { return slot.id == id; });
        return found == raw_->end() ? nullptr : &*found;
    }
    const auto* record = find_record(store_->slots, id);
    return record ? &record->slot : nullptr;
}

SlotList::Iterator SlotList::begin() const noexcept {
    return raw_ ? Iterator(raw_, {}, 0, {}) : Iterator({}, store_, 0, store_->head);
}

SlotList::Iterator SlotList::end() const noexcept {
    return raw_ ? Iterator(raw_, {}, raw_->size(), {}) : Iterator({}, store_, 0, {});
}

const Slot& SlotList::Iterator::operator*() const noexcept {
    return raw_ ? (*raw_)[raw_index_] : find_record(store_->slots, current_)->slot;
}

const Slot* SlotList::Iterator::operator->() const noexcept {
    return &operator*();
}

SlotList::Iterator& SlotList::Iterator::operator++() noexcept {
    if (raw_)
        ++raw_index_;
    else
        current_ = find_record(store_->slots, current_)->next;
    return *this;
}

bool SlotList::shares_storage_with(const SlotList& other) const noexcept {
    return (raw_ && raw_ == other.raw_) || (store_ && store_ == other.store_);
}

bool SlotList::operator==(const SlotList& other) const noexcept {
    return size() == other.size() && std::equal(begin(), end(), other.begin());
}

namespace detail {

class LauncherStore {
  public:
    NodePtr<SceneRecord> scenes;
    NodePtr<IdRecord> slot_owners;
    NodePtr<ReferenceRecord> clip_references;
    NodePtr<ReferenceRecord> jump_references;
    ItemId head;
    ItemId tail;
    std::size_t scene_count = 0;
    std::size_t slot_count = 0;

    const SceneRecord* find_scene(ItemId id) const noexcept {
        return find_record(scenes, id);
    }
    const IdRecord* find_slot_owner(ItemId id) const noexcept {
        return find_record(slot_owners, id);
    }
    const ReferenceRecord* clip_sources(ItemId id) const noexcept {
        return find_record(clip_references, id);
    }
    const ReferenceRecord* jump_sources(ItemId id) const noexcept {
        return find_record(jump_references, id);
    }
};

} // namespace detail

namespace detail {

runtime::Result<std::shared_ptr<const LauncherStore>, ModelError>
build_launcher(std::vector<Scene> scenes, std::span<const Track> tracks) {
    std::vector<ItemId> owned_ids;
    std::vector<SceneRecord> scene_records;
    std::vector<IdRecord> slot_owners;
    owned_ids.reserve(scenes.size());
    scene_records.reserve(scenes.size());
    ItemId previous_scene;
    for (auto& scene : scenes) {
        if (!scene.id.valid())
            return fail<std::shared_ptr<const LauncherStore>>(ModelErrorCode::InvalidItemId,
                                                              scene.id);
        auto slots = detail::SlotListAccess::make_persistent(scene.slots, scene.id);
        if (!slots)
            return runtime::Err(slots.error());
        scene.slots = std::move(slots).value();
        owned_ids.push_back(scene.id);
        scene_records.push_back({scene.id, scene, previous_scene, {}});
        if (scene_records.size() > 1)
            scene_records[scene_records.size() - 2].next = scene.id;
        previous_scene = scene.id;
        for (const auto& slot : scene.slots) {
            if (slot.id == scene.id)
                return fail<std::shared_ptr<const LauncherStore>>(ModelErrorCode::DuplicateItemId,
                                                                  slot.id, scene.id);
            owned_ids.push_back(slot.id);
            slot_owners.push_back({slot.id, scene.id});
        }
    }
    std::sort(owned_ids.begin(), owned_ids.end());
    if (const auto duplicate = std::adjacent_find(owned_ids.begin(), owned_ids.end());
        duplicate != owned_ids.end())
        return fail<std::shared_ptr<const LauncherStore>>(ModelErrorCode::DuplicateItemId,
                                                          *duplicate);
    std::sort(slot_owners.begin(), slot_owners.end(),
              [](const IdRecord& lhs, const IdRecord& rhs) { return lhs.id < rhs.id; });

    auto store = std::make_shared<LauncherStore>();
    auto by_scene_id = scene_records;
    std::sort(by_scene_id.begin(), by_scene_id.end(),
              [](const SceneRecord& lhs, const SceneRecord& rhs) { return lhs.id < rhs.id; });
    store->scenes = build_balanced<SceneRecord>(by_scene_id, 0, by_scene_id.size());
    store->slot_owners = build_balanced<IdRecord>(slot_owners, 0, slot_owners.size());
    store->scene_count = scene_records.size();
    store->slot_count = slot_owners.size();
    if (!scene_records.empty()) {
        store->head = scene_records.front().id;
        store->tail = scene_records.back().id;
    }

    for (const auto& scene : scenes) {
        for (const auto& slot : scene.slots) {
            if (slot.clip_id.valid()) {
                if (!tracks_contain_clip(tracks, slot.clip_id))
                    return fail<std::shared_ptr<const LauncherStore>>(ModelErrorCode::MissingItem,
                                                                      slot.clip_id, slot.id);
                store->clip_references =
                    reference_add(std::move(store->clip_references), slot.clip_id, slot.id);
            }
            for (const auto& action : slot.follow.active()) {
                if (action.kind != FollowActionKind::Jump)
                    continue;
                if (!store->find_slot_owner(action.target))
                    return fail<std::shared_ptr<const LauncherStore>>(ModelErrorCode::MissingItem,
                                                                      action.target, slot.id);
                store->jump_references =
                    reference_add(std::move(store->jump_references), action.target, slot.id);
            }
        }
    }
    return runtime::Ok(std::shared_ptr<const LauncherStore>(std::move(store)));
}

std::optional<ModelError> validate_scene_for_insert(const LauncherStore& store, const Scene& scene,
                                                    std::span<const Track> tracks) {
    if (!scene.id.valid())
        return ModelError{ModelErrorCode::InvalidItemId, scene.id, {}};
    if (store.find_scene(scene.id) || store.find_slot_owner(scene.id))
        return ModelError{ModelErrorCode::DuplicateItemId, scene.id, {}};
    for (const auto& slot : scene.slots) {
        if (slot.id == scene.id || store.find_scene(slot.id) || store.find_slot_owner(slot.id))
            return ModelError{ModelErrorCode::DuplicateItemId, slot.id, scene.id};
        if (slot.clip_id.valid() && !tracks_contain_clip(tracks, slot.clip_id))
            return ModelError{ModelErrorCode::MissingItem, slot.clip_id, slot.id};
        for (const auto& action : slot.follow.active())
            if (action.kind == FollowActionKind::Jump && !store.find_slot_owner(action.target) &&
                !scene.slots.find(action.target))
                return ModelError{ModelErrorCode::MissingItem, action.target, slot.id};
    }
    return std::nullopt;
}

runtime::Result<std::shared_ptr<const LauncherStore>, ModelError>
insert_scene_store(const std::shared_ptr<const LauncherStore>& source, Scene scene,
                   std::optional<ItemId> before, std::span<const Track> tracks) {
    if (before && !source->find_scene(*before))
        return fail<std::shared_ptr<const LauncherStore>>(ModelErrorCode::MissingItem, *before);
    auto slots = detail::SlotListAccess::make_persistent(scene.slots, scene.id);
    if (!slots)
        return runtime::Err(slots.error());
    scene.slots = std::move(slots).value();
    if (const auto invalid = validate_scene_for_insert(*source, scene, tracks))
        return runtime::Err(*invalid);

    auto updated = std::make_shared<LauncherStore>(*source);
    const auto* next = before ? source->find_scene(*before) : nullptr;
    const ItemId previous = next ? next->previous : source->tail;
    if (previous.valid()) {
        auto record = *find_record(updated->scenes, previous);
        record.next = scene.id;
        bool inserted = false;
        updated->scenes = put(std::move(updated->scenes), std::move(record), inserted);
    } else {
        updated->head = scene.id;
    }
    if (next) {
        auto record = *next;
        record.previous = scene.id;
        bool inserted = false;
        updated->scenes = put(std::move(updated->scenes), std::move(record), inserted);
    } else {
        updated->tail = scene.id;
    }
    bool inserted = false;
    updated->scenes =
        put(std::move(updated->scenes),
            SceneRecord{scene.id, scene, previous, next ? next->id : ItemId{}}, inserted);
    ++updated->scene_count;
    for (const auto& slot : scene.slots) {
        inserted = false;
        updated->slot_owners =
            put(std::move(updated->slot_owners), IdRecord{slot.id, scene.id}, inserted);
        ++updated->slot_count;
        if (slot.clip_id.valid())
            updated->clip_references =
                reference_add(std::move(updated->clip_references), slot.clip_id, slot.id);
        for (const auto& action : slot.follow.active())
            if (action.kind == FollowActionKind::Jump)
                updated->jump_references =
                    reference_add(std::move(updated->jump_references), action.target, slot.id);
    }
    return runtime::Ok(std::shared_ptr<const LauncherStore>(std::move(updated)));
}

std::optional<ItemId> external_jump_source(const LauncherStore& store, const Scene& scene,
                                           ItemId target) {
    const auto* references = store.jump_sources(target);
    if (!references)
        return std::nullopt;
    std::vector<const PersistentNode<SetRecord>*> stack;
    auto current = references->sources.get();
    while (current || !stack.empty()) {
        while (current) {
            stack.push_back(current);
            current = current->left.get();
        }
        current = stack.back();
        stack.pop_back();
        const auto source = current->record.id;
        const auto* owner = store.find_slot_owner(source);
        if (!owner || owner->value != scene.id)
            return source;
        current = current->right.get();
    }
    return std::nullopt;
}

std::optional<ItemId> external_reference_source(const NodePtr<SetRecord>& sources, ItemId ignored) {
    if (!sources)
        return std::nullopt;
    if (sources->record.id != ignored)
        return sources->record.id;
    if (const auto left = external_reference_source(sources->left, ignored))
        return left;
    return external_reference_source(sources->right, ignored);
}

runtime::Result<std::shared_ptr<const LauncherStore>, ModelError>
erase_scene_store(const std::shared_ptr<const LauncherStore>& source, ItemId id) {
    const auto* removed = source->find_scene(id);
    if (!removed)
        return fail<std::shared_ptr<const LauncherStore>>(ModelErrorCode::MissingItem, id);
    for (const auto& slot : removed->scene.slots)
        if (const auto external = external_jump_source(*source, removed->scene, slot.id))
            return fail<std::shared_ptr<const LauncherStore>>(ModelErrorCode::MissingItem, slot.id,
                                                              *external);

    auto updated = std::make_shared<LauncherStore>(*source);
    if (removed->previous.valid()) {
        auto previous = *find_record(updated->scenes, removed->previous);
        previous.next = removed->next;
        bool inserted = false;
        updated->scenes = put(std::move(updated->scenes), std::move(previous), inserted);
    } else {
        updated->head = removed->next;
    }
    if (removed->next.valid()) {
        auto next = *find_record(updated->scenes, removed->next);
        next.previous = removed->previous;
        bool inserted = false;
        updated->scenes = put(std::move(updated->scenes), std::move(next), inserted);
    } else {
        updated->tail = removed->previous;
    }
    for (const auto& slot : removed->scene.slots) {
        if (slot.clip_id.valid())
            updated->clip_references =
                reference_remove(std::move(updated->clip_references), slot.clip_id, slot.id);
        for (const auto& action : slot.follow.active())
            if (action.kind == FollowActionKind::Jump)
                updated->jump_references =
                    reference_remove(std::move(updated->jump_references), action.target, slot.id);
        bool erased = false;
        updated->slot_owners = erase_record(std::move(updated->slot_owners), slot.id, erased);
        --updated->slot_count;
    }
    bool erased = false;
    updated->scenes = erase_record(std::move(updated->scenes), id, erased);
    --updated->scene_count;
    return runtime::Ok(std::shared_ptr<const LauncherStore>(std::move(updated)));
}

runtime::Result<std::shared_ptr<const LauncherStore>, ModelError>
insert_slot_store(const std::shared_ptr<const LauncherStore>& source, ItemId scene_id, Slot slot,
                  std::optional<ItemId> before, std::span<const Track> tracks) {
    const auto* scene_record = source->find_scene(scene_id);
    if (!scene_record)
        return fail<std::shared_ptr<const LauncherStore>>(ModelErrorCode::MissingItem, scene_id);
    if (source->find_scene(slot.id) || source->find_slot_owner(slot.id))
        return fail<std::shared_ptr<const LauncherStore>>(ModelErrorCode::DuplicateItemId, slot.id,
                                                          scene_id);
    if (slot.clip_id.valid() && !tracks_contain_clip(tracks, slot.clip_id))
        return fail<std::shared_ptr<const LauncherStore>>(ModelErrorCode::MissingItem, slot.clip_id,
                                                          slot.id);
    for (const auto& action : slot.follow.active())
        if (action.kind == FollowActionKind::Jump && action.target != slot.id &&
            !source->find_slot_owner(action.target))
            return fail<std::shared_ptr<const LauncherStore>>(ModelErrorCode::MissingItem,
                                                              action.target, slot.id);
    auto slots = detail::SlotListAccess::insert(scene_record->scene.slots, slot, before, scene_id);
    if (!slots)
        return runtime::Err(slots.error());
    auto updated = std::make_shared<LauncherStore>(*source);
    auto replacement = *scene_record;
    replacement.scene.slots = std::move(slots).value();
    bool inserted = false;
    updated->scenes = put(std::move(updated->scenes), std::move(replacement), inserted);
    updated->slot_owners =
        put(std::move(updated->slot_owners), IdRecord{slot.id, scene_id}, inserted);
    ++updated->slot_count;
    if (slot.clip_id.valid())
        updated->clip_references =
            reference_add(std::move(updated->clip_references), slot.clip_id, slot.id);
    for (const auto& action : slot.follow.active())
        if (action.kind == FollowActionKind::Jump)
            updated->jump_references =
                reference_add(std::move(updated->jump_references), action.target, slot.id);
    return runtime::Ok(std::shared_ptr<const LauncherStore>(std::move(updated)));
}

runtime::Result<std::shared_ptr<const LauncherStore>, ModelError>
erase_slot_store(const std::shared_ptr<const LauncherStore>& source, ItemId scene_id,
                 ItemId slot_id) {
    const auto* scene_record = source->find_scene(scene_id);
    const auto* slot = scene_record ? scene_record->scene.slots.find(slot_id) : nullptr;
    if (!slot)
        return fail<std::shared_ptr<const LauncherStore>>(ModelErrorCode::MissingItem, slot_id,
                                                          scene_id);
    if (const auto* references = source->jump_sources(slot_id))
        if (const auto source_id = external_reference_source(references->sources, slot_id))
            return fail<std::shared_ptr<const LauncherStore>>(ModelErrorCode::MissingItem, slot_id,
                                                              *source_id);
    auto slots = detail::SlotListAccess::erase(scene_record->scene.slots, slot_id, scene_id);
    if (!slots)
        return runtime::Err(slots.error());
    auto updated = std::make_shared<LauncherStore>(*source);
    auto replacement = *scene_record;
    replacement.scene.slots = std::move(slots).value();
    bool inserted = false;
    updated->scenes = put(std::move(updated->scenes), std::move(replacement), inserted);
    if (slot->clip_id.valid())
        updated->clip_references =
            reference_remove(std::move(updated->clip_references), slot->clip_id, slot_id);
    for (const auto& action : slot->follow.active())
        if (action.kind == FollowActionKind::Jump)
            updated->jump_references =
                reference_remove(std::move(updated->jump_references), action.target, slot_id);
    bool erased = false;
    updated->slot_owners = erase_record(std::move(updated->slot_owners), slot_id, erased);
    --updated->slot_count;
    return runtime::Ok(std::shared_ptr<const LauncherStore>(std::move(updated)));
}

std::size_t launcher_scene_count(const LauncherStore& store) noexcept {
    return store.scene_count;
}

ItemId launcher_head(const LauncherStore& store) noexcept {
    return store.head;
}

const Scene* launcher_find_scene(const LauncherStore& store, ItemId id) noexcept {
    const auto* record = store.find_scene(id);
    return record ? &record->scene : nullptr;
}

ItemId launcher_next_scene(const LauncherStore& store, ItemId id) noexcept {
    const auto* record = store.find_scene(id);
    return record ? record->next : ItemId{};
}

const Slot* launcher_find_slot(const LauncherStore& store, ItemId id) noexcept {
    const auto* owner = store.find_slot_owner(id);
    const auto* scene = owner ? store.find_scene(owner->value) : nullptr;
    return scene ? scene->scene.slots.find(id) : nullptr;
}

std::optional<ItemId> launcher_clip_source(const LauncherStore& store, ItemId clip_id) noexcept {
    const auto* references = store.clip_sources(clip_id);
    const auto* source = references ? minimum(references->sources) : nullptr;
    return source ? std::optional<ItemId>(source->record.id) : std::nullopt;
}

LauncherIndexStats launcher_index_stats() noexcept {
    return {g_live_launcher_nodes.load(), g_created_launcher_nodes.load()};
}

std::size_t shared_launcher_nodes(const LauncherStore& lhs, const LauncherStore& rhs) {
    std::unordered_set<const void*> addresses;
    collect_addresses(lhs.scenes, addresses);
    collect_addresses(lhs.slot_owners, addresses);
    collect_addresses(lhs.clip_references, addresses);
    collect_addresses(lhs.jump_references, addresses);
    std::size_t shared =
        count_shared(rhs.scenes, addresses) + count_shared(rhs.slot_owners, addresses) +
        count_shared(rhs.clip_references, addresses) + count_shared(rhs.jump_references, addresses);
    auto current = lhs.head;
    while (current.valid()) {
        const auto* scene = lhs.find_scene(current);
        const auto* other_scene = rhs.find_scene(current);
        if (other_scene) {
            const auto* slots = SlotListAccess::store(scene->scene.slots);
            const auto* other_slots = SlotListAccess::store(other_scene->scene.slots);
            if (slots && other_slots) {
                addresses.clear();
                collect_addresses(slots->slots, addresses);
                shared += count_shared(other_slots->slots, addresses);
            }
        }
        current = scene->next;
    }
    return shared;
}

} // namespace detail

} // namespace pulp::timeline
