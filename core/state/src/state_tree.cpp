#include <pulp/state/state_tree.hpp>
#include <choc/text/choc_JSON.h>

namespace pulp::state {

void StateTree::set(std::string_view name, PropertyValue value) {
    std::string key(name);
    auto old = properties_.count(key) ? properties_[key] : PropertyValue{};
    properties_[key] = value;
    notify_property_changed(name, old, value);
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
    properties_.erase(key);
}

std::vector<std::string> StateTree::property_names() const {
    std::vector<std::string> names;
    for (auto& [k, v] : properties_) names.push_back(k);
    return names;
}

void StateTree::add_child(Ptr child) {
    if (!child) return;
    child->parent_ = this;
    children_.push_back(child);
    int idx = static_cast<int>(children_.size()) - 1;
    for (auto& [id, fn] : child_added_listeners_)
        if (fn) fn(*this, *child, idx);
}

void StateTree::insert_child(int index, Ptr child) {
    if (!child) return;
    index = std::clamp(index, 0, child_count());
    child->parent_ = this;
    children_.insert(children_.begin() + index, child);
    for (auto& [id, fn] : child_added_listeners_)
        if (fn) fn(*this, *child, index);
}

void StateTree::remove_child(int index) {
    if (index < 0 || index >= child_count()) return;
    auto child = children_[index];
    child->parent_ = nullptr;
    for (auto& [id, fn] : child_removed_listeners_)
        if (fn) fn(*this, *child, index);
    children_.erase(children_.begin() + index);
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

void StateTree::notify_property_changed(std::string_view name,
                                        const PropertyValue& old_val,
                                        const PropertyValue& new_val) {
    for (auto& [id, fn] : listeners_)
        if (fn) fn(*this, name, old_val, new_val);
}

static choc::value::Value tree_to_choc(const StateTree& node) {
    auto obj = choc::value::createObject("StateTreeNode");
    obj.addMember("type", node.type_name());

    // Properties
    auto props = choc::value::createObject("properties");
    for (auto& name : node.property_names()) {
        auto val = node.get(name);
        if (auto* b = std::get_if<bool>(&val))
            props.addMember(name, *b);
        else if (auto* i = std::get_if<int64_t>(&val))
            props.addMember(name, *i);
        else if (auto* d = std::get_if<double>(&val))
            props.addMember(name, *d);
        else if (auto* s = std::get_if<std::string>(&val))
            props.addMember(name, *s);
    }
    if (node.property_names().size() > 0)
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
            std::string key(member.name);
            if (member.value.isBool())
                node->set(key, member.value.getBool());
            else if (member.value.isFloat64())
                node->set(key, member.value.getFloat64());
            else if (member.value.isInt64())
                node->set(key, member.value.getInt64());
            else if (member.value.isInt32())
                node->set(key, static_cast<int64_t>(member.value.getInt32()));
            else if (member.value.isString())
                node->set(key, std::string(member.value.getString()));
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
    copy->properties_ = properties_;
    for (auto& child : children_)
        copy->add_child(child->deep_copy());
    return copy;
}

}  // namespace pulp::state
