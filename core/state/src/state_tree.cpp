#include <pulp/state/state_tree.hpp>
#include <choc/text/choc_JSON.h>

namespace pulp::state {

namespace {

PropertyValue clone_property_value(const PropertyValue& value);

PropertyValue clone_property_array(const PropertyArray& array) {
    std::vector<PropertyValue> values;
    values.reserve(array.values.size());
    for (const auto& element : array.values)
        values.push_back(clone_property_value(element));
    return make_property_array(std::move(values));
}

PropertyValue clone_property_object(const PropertyObject& object) {
    std::map<std::string, PropertyValue, std::less<>> values;
    for (const auto& [key, member] : object.values)
        values.emplace(key, clone_property_value(member));
    return make_property_object(std::move(values));
}

PropertyValue clone_property_value(const PropertyValue& value) {
    if (const auto* array = get_property_array(value))
        return clone_property_array(*array);
    if (const auto* object = get_property_object(value))
        return clone_property_object(*object);
    return value;
}

}  // namespace

void StateTree::set(std::string_view name, PropertyValue value) {
    std::string key(name);
    auto old = properties_.count(key) ? properties_[key] : PropertyValue{};
    auto stored = clone_property_value(value);
    properties_[key] = stored;
    notify_property_changed(name, old, stored);
}

PropertyValue StateTree::get(std::string_view name) const {
    auto it = properties_.find(name);
    return it != properties_.end() ? it->second : PropertyValue{};
}

bool StateTree::get_bool(std::string_view name, bool d) const {
    auto v = get(name);
    if (auto* b = std::get_if<bool>(&v)) return *b;
    return d;
}

int64_t StateTree::get_int(std::string_view name, int64_t d) const {
    auto v = get(name);
    if (auto* i = std::get_if<int64_t>(&v)) return *i;
    return d;
}

double StateTree::get_double(std::string_view name, double d) const {
    auto v = get(name);
    if (auto* f = std::get_if<double>(&v)) return *f;
    // JSON may deserialize whole-number doubles as int64
    if (auto* i = std::get_if<int64_t>(&v)) return static_cast<double>(*i);
    return d;
}

std::string StateTree::get_string(std::string_view name, std::string_view d) const {
    auto v = get(name);
    if (auto* s = std::get_if<std::string>(&v)) return *s;
    return std::string(d);
}

bool StateTree::has(std::string_view name) const {
    return properties_.find(name) != properties_.end();
}

void StateTree::remove(std::string_view name) {
    std::string key(name);
    auto it = properties_.find(key);
    if (it == properties_.end()) return;
    auto old = it->second;
    properties_.erase(it);
    notify_property_changed(name, old, PropertyValue{});
}

std::vector<std::string> StateTree::property_names() const {
    std::vector<std::string> names;
    for (auto& [k, v] : properties_) names.push_back(k);
    return names;
}

bool StateTree::is_ancestor_of(const StateTree* node) const {
    for (const StateTree* p = node; p != nullptr; p = p->parent_)
        if (p == this) return true;
    return false;
}

void StateTree::add_child(Ptr child) {
    if (!child) return;
    // Reject anything that would form a parent/child cycle: a node cannot
    // become its own descendant. Adding `child` under `this` is illegal iff
    // `child` is `this` or `child` is already an ancestor of `this`. Such a
    // cycle leaks a shared_ptr ring and makes deep_copy() / to_json() recurse
    // forever. Leave the tree unchanged and fire no listeners on rejection.
    if (child.get() == this || child->is_ancestor_of(this)) return;
    if (child->parent_ != nullptr)
        child->parent_->remove_child(child.get());
    child->parent_ = this;
    children_.push_back(child);
    int idx = static_cast<int>(children_.size()) - 1;
    notify_child_added(*child, idx);
}

void StateTree::insert_child(int index, Ptr child) {
    if (!child) return;
    // Same cycle guard as add_child — see the comment there.
    if (child.get() == this || child->is_ancestor_of(this)) return;
    if (child->parent_ != nullptr)
        child->parent_->remove_child(child.get());
    index = std::clamp(index, 0, child_count());
    child->parent_ = this;
    children_.insert(children_.begin() + index, child);
    notify_child_added(*child, index);
}

void StateTree::remove_child(int index) {
    if (index < 0 || index >= child_count()) return;
    // Copy the shared_ptr so the child stays alive while listeners run, then
    // erase before dispatch so a re-entrant mutation from a callback sees the
    // tree in its post-removal shape.
    auto child = children_[index];
    child->parent_ = nullptr;
    children_.erase(children_.begin() + index);
    notify_child_removed(*child, index);
}

void StateTree::remove_child(StateTree* child) {
    for (int i = 0; i < child_count(); ++i) {
        if (children_[i].get() == child) {
            remove_child(i);
            return;
        }
    }
}

StateTree::Ptr StateTree::child(int index) const {
    if (index < 0 || index >= child_count()) return nullptr;
    return children_[index];
}

StateTree::Ptr StateTree::find_child(std::string_view type_name) const {
    for (auto& c : children_)
        if (c->type_name() == type_name) return c;
    return nullptr;
}

std::vector<StateTree::Ptr> StateTree::find_children(std::string_view type_name) const {
    std::vector<Ptr> result;
    for (auto& c : children_)
        if (c->type_name() == type_name) result.push_back(c);
    return result;
}

int StateTree::add_listener(TreeListener listener) {
    int id = next_listener_id_++;
    listeners_.push_back({id, std::move(listener)});
    return id;
}

void StateTree::remove_listener(int id) {
    listeners_.erase(
        std::remove_if(listeners_.begin(), listeners_.end(),
                      [id](auto& e) { return e.id == id; }),
        listeners_.end());
}

int StateTree::add_child_added_listener(ChildListener listener) {
    int id = next_listener_id_++;
    child_added_listeners_.push_back({id, std::move(listener)});
    return id;
}

int StateTree::add_child_removed_listener(ChildListener listener) {
    int id = next_listener_id_++;
    child_removed_listeners_.push_back({id, std::move(listener)});
    return id;
}

void StateTree::remove_child_added_listener(int id) {
    child_added_listeners_.erase(
        std::remove_if(child_added_listeners_.begin(), child_added_listeners_.end(),
                      [id](auto& e) { return e.first == id; }),
        child_added_listeners_.end());
}

void StateTree::remove_child_removed_listener(int id) {
    child_removed_listeners_.erase(
        std::remove_if(child_removed_listeners_.begin(), child_removed_listeners_.end(),
                      [id](auto& e) { return e.first == id; }),
        child_removed_listeners_.end());
}

// Listener fan-out is snapshot-then-dispatch with a liveness recheck. A
// callback may register or unregister listeners (or add/remove children) on
// this same node while it runs, which would invalidate iterators/references
// into the live backing vectors. We copy the {id, fn} entries up front, then
// before each call confirm the id is STILL registered — so a listener removed
// earlier in the same dispatch is not invoked, and a listener added during the
// dispatch is not invoked for the in-flight notification. The single-listener
// case (the common one) takes a fast path that skips the copy.

void StateTree::notify_property_changed(std::string_view name,
                                        const PropertyValue& old_val,
                                        const PropertyValue& new_val) {
    if (listeners_.size() == 1) {
        // Copy the callable before invoking: a sole listener that removes
        // itself mid-callback would otherwise free the std::function whose
        // body is still executing (heap-use-after-free for heap-captured
        // callables). The copy keeps the executing closure alive.
        auto fn = listeners_.front().fn;
        if (fn) fn(*this, name, old_val, new_val);
        return;
    }
    auto snapshot = listeners_;
    for (auto& [id, fn] : snapshot) {
        if (!fn) continue;
        bool still_registered = std::any_of(
            listeners_.begin(), listeners_.end(),
            [id = id](const ListenerEntry& e) { return e.id == id; });
        if (still_registered) fn(*this, name, old_val, new_val);
    }
}

void StateTree::notify_child_added(StateTree& child, int index) {
    if (child_added_listeners_.size() == 1) {
        // Copy before invoking — see notify_property_changed for why.
        auto fn = child_added_listeners_.front().second;
        if (fn) fn(*this, child, index);
        return;
    }
    auto snapshot = child_added_listeners_;
    for (auto& [id, fn] : snapshot) {
        if (!fn) continue;
        bool still_registered = std::any_of(
            child_added_listeners_.begin(), child_added_listeners_.end(),
            [id = id](const auto& e) { return e.first == id; });
        if (still_registered) fn(*this, child, index);
    }
}

void StateTree::notify_child_removed(StateTree& child, int index) {
    if (child_removed_listeners_.size() == 1) {
        // Copy before invoking — see notify_property_changed for why.
        auto fn = child_removed_listeners_.front().second;
        if (fn) fn(*this, child, index);
        return;
    }
    auto snapshot = child_removed_listeners_;
    for (auto& [id, fn] : snapshot) {
        if (!fn) continue;
        bool still_registered = std::any_of(
            child_removed_listeners_.begin(), child_removed_listeners_.end(),
            [id = id](const auto& e) { return e.first == id; });
        if (still_registered) fn(*this, child, index);
    }
}

static choc::value::Value property_to_choc(const PropertyValue& value) {
    if (auto* b = std::get_if<bool>(&value))
        return choc::value::Value(*b);
    if (auto* i = std::get_if<int64_t>(&value))
        return choc::value::Value(*i);
    if (auto* d = std::get_if<double>(&value))
        return choc::value::Value(*d);
    if (auto* str = std::get_if<std::string>(&value))
        return choc::value::Value(*str);
    if (const auto* array = get_property_array(value)) {
        auto arr = choc::value::createEmptyArray();
        for (const auto& element : array->values)
            arr.addArrayElement(property_to_choc(element));
        return arr;
    }
    if (const auto* object = get_property_object(value)) {
        auto obj = choc::value::createObject("PropertyObject");
        for (const auto& [key, member] : object->values)
            obj.addMember(key, property_to_choc(member));
        return obj;
    }
    return choc::value::Value();
}

static std::optional<PropertyValue> choc_to_property(const choc::value::ValueView& value,
                                                     bool allow_null) {
    if (value.isVoid())
        return allow_null ? std::optional<PropertyValue>(PropertyValue{}) : std::nullopt;
    if (value.isBool()) return PropertyValue(value.getBool());
    if (value.isFloat64()) return PropertyValue(value.getFloat64());
    if (value.isInt64()) return PropertyValue(value.getInt64());
    if (value.isInt32()) return PropertyValue(static_cast<int64_t>(value.getInt32()));
    if (value.isString()) return PropertyValue(std::string(value.getString()));
    if (value.isArray()) {
        std::vector<PropertyValue> values;
        values.reserve(value.size());
        for (uint32_t i = 0; i < value.size(); ++i) {
            auto element = choc_to_property(value[i], true);
            if (!element) return std::nullopt;
            values.push_back(std::move(*element));
        }
        return make_property_array(std::move(values));
    }
    if (value.isObject()) {
        std::map<std::string, PropertyValue, std::less<>> values;
        for (uint32_t i = 0; i < value.size(); ++i) {
            auto member = value.getObjectMemberAt(i);
            auto member_value = choc_to_property(member.value, true);
            if (!member_value) return std::nullopt;
            values.emplace(std::string(member.name), std::move(*member_value));
        }
        return make_property_object(std::move(values));
    }
    return std::nullopt;
}

static choc::value::Value tree_to_choc(const StateTree& node) {
    auto obj = choc::value::createObject("StateTreeNode");
    obj.addMember("type", node.type_name());

    // Properties
    auto props = choc::value::createObject("properties");
    bool has_props = false;
    for (auto& name : node.property_names()) {
        auto val = node.get(name);
        // Preserve existing scalar behavior: an explicit top-level property
        // monostate serializes as absent, while nested arrays/objects may still
        // contain JSON null values.
        if (std::holds_alternative<std::monostate>(val))
            continue;
        props.addMember(name, property_to_choc(val));
        has_props = true;
    }
    if (has_props)
        obj.addMember("properties", props);

    // Children
    if (node.child_count() > 0) {
        auto arr = choc::value::createEmptyArray();
        for (int i = 0; i < node.child_count(); ++i)
            arr.addArrayElement(tree_to_choc(*node.child(i)));
        obj.addMember("children", arr);
    }

    return obj;
}

static StateTree::Ptr choc_to_tree(const choc::value::ValueView& val) {
    if (!val.isObject()) return nullptr;

    std::string type_name = "node";
    if (val.hasObjectMember("type") && val["type"].isString())
        type_name = std::string(val["type"].getString());

    auto node = StateTree::create(type_name);

    // Properties
    if (val.hasObjectMember("properties") && val["properties"].isObject()) {
        auto props = val["properties"];
        for (uint32_t i = 0; i < props.size(); ++i) {
            auto member = props.getObjectMemberAt(i);
            if (auto property = choc_to_property(member.value, false))
                node->set(std::string(member.name), std::move(*property));
        }
    }

    // Children
    if (val.hasObjectMember("children") && val["children"].isArray()) {
        auto children = val["children"];
        for (uint32_t i = 0; i < children.size(); ++i) {
            auto child = choc_to_tree(children[i]);
            if (child) node->add_child(child);
        }
    }

    return node;
}

std::string StateTree::to_json() const {
    auto val = tree_to_choc(*this);
    return choc::json::toString(val, true);
}

StateTree::Ptr StateTree::from_json(std::string_view json) {
    try {
        auto val = choc::json::parse(json);
        return choc_to_tree(val);
    } catch (...) {
        return nullptr;
    }
}

StateTree::Ptr StateTree::deep_copy() const {
    auto copy = create(type_name_);
    for (const auto& [key, value] : properties_)
        copy->properties_.emplace(key, clone_property_value(value));
    for (auto& child : children_)
        copy->add_child(child->deep_copy());
    return copy;
}

StateTree::SyncedClone StateTree::clone_synced() {
    return SyncedClone(shared_from_this(), deep_copy());
}

// ── SyncedClone ─────────────────────────────────────────────────────────
//
// Single-process equivalent of StateTreeSynchroniser. Walks the source +
// clone in parallel, installs property + child listeners at every level
// of the source, and applies the equivalent mutation on the matching
// clone node. Newly added children on the source side are deep-copied
// onto the clone AND recursively wired so the observer keeps mirroring
// after structural changes.

StateTree::SyncedClone::SyncedClone(StateTree::Ptr source,
                                    StateTree::Ptr cloned)
    : source_(std::move(source)),
      clone_(std::move(cloned)) {
    attach_recursive(*source_, *clone_);
}

StateTree::SyncedClone::SyncedClone(SyncedClone&& other) noexcept
    : source_(std::move(other.source_)),
      clone_(std::move(other.clone_)),
      wiring_(std::move(other.wiring_)),
      attached_(other.attached_) {
    other.attached_ = false;
}

StateTree::SyncedClone&
StateTree::SyncedClone::operator=(SyncedClone&& other) noexcept {
    if (this != &other) {
        detach();
        source_ = std::move(other.source_);
        clone_ = std::move(other.clone_);
        wiring_ = std::move(other.wiring_);
        attached_ = other.attached_;
        other.attached_ = false;
    }
    return *this;
}

StateTree::SyncedClone::~SyncedClone() {
    detach();
}

void StateTree::SyncedClone::detach() {
    if (!attached_) return;
    for (auto& w : wiring_) {
        // The source subtree may have been removed via `remove_child`
        // and dropped to refcount 0 between attach time and detach. In
        // that case `lock()` returns null and the listener vector is
        // already gone with the StateTree, so there is nothing to
        // remove. Pinned by the dangling-pointer regression test.
        if (auto src = w.source.lock()) {
            src->remove_listener(w.prop_listener_id);
            src->remove_child_added_listener(w.child_added_listener_id);
            src->remove_child_removed_listener(w.child_removed_listener_id);
        }
    }
    wiring_.clear();
    attached_ = false;
}

void StateTree::SyncedClone::attach_recursive(StateTree& src, StateTree& dst) {
    // Property changes: set / remove on src → set / remove on dst.
    int prop_id = src.add_listener(
        [dst_ptr = &dst](StateTree& /*node*/, std::string_view prop,
                         const PropertyValue& /*old*/, const PropertyValue& new_v) {
            if (std::holds_alternative<std::monostate>(new_v)) {
                dst_ptr->remove(prop);
            } else {
                dst_ptr->set(prop, new_v);
            }
        });

    // Child-added on src → deep-copy onto dst at the same index AND wire
    // the new subtree recursively so deeper mutations keep mirroring.
    int added_id = src.add_child_added_listener(
        [this, dst_ptr = &dst](StateTree& /*parent*/,
                               StateTree& added_child, int idx) {
            auto cloned_child = added_child.deep_copy();
            dst_ptr->insert_child(idx, cloned_child);
            this->attach_recursive(added_child, *cloned_child);
        });

    // Child-removed on src → remove same index on dst. We don't bother
    // de-registering the listeners we installed on the removed subtree's
    // source nodes — the wiring vector keeps non-owning pointers that
    // will simply no-op when the source subtree is destroyed (its
    // listener vectors die with it). If the caller re-parents the
    // removed subtree later that's a separate `clone_synced` call.
    int removed_id = src.add_child_removed_listener(
        [dst_ptr = &dst](StateTree& /*parent*/, StateTree& /*removed_child*/,
                         int idx) {
            dst_ptr->remove_child(idx);
        });

    // weak_ptr so dropped subtrees don't dangle here — see WiringEntry
    // comment in the header.
    wiring_.push_back({src.weak_from_this(),
                       prop_id, added_id, removed_id});

    // Recurse on existing children. dst was deep-copied from src in the
    // same shape, so positional zip is safe.
    int n = src.child_count();
    int dst_n = dst.child_count();
    int common = std::min(n, dst_n);
    for (int i = 0; i < common; ++i) {
        auto src_child = src.child(i);
        auto dst_child = dst.child(i);
        if (src_child && dst_child) {
            attach_recursive(*src_child, *dst_child);
        }
    }
}

}  // namespace pulp::state
