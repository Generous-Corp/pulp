#include <pulp/state/state_tree.hpp>

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
    child->parent_ = this;
    children_.push_back(child);
    int idx = static_cast<int>(children_.size()) - 1;
    for (auto& [id, fn] : child_added_listeners_)
        fn(*this, *child, idx);
}

void StateTree::insert_child(int index, Ptr child) {
    child->parent_ = this;
    children_.insert(children_.begin() + index, child);
    for (auto& [id, fn] : child_added_listeners_)
        fn(*this, *child, index);
}

void StateTree::remove_child(int index) {
    if (index < 0 || index >= child_count()) return;
    auto child = children_[index];
    child->parent_ = nullptr;
    for (auto& [id, fn] : child_removed_listeners_)
        fn(*this, *child, index);
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

void StateTree::notify_property_changed(std::string_view name,
                                        const PropertyValue& old_val,
                                        const PropertyValue& new_val) {
    for (auto& [id, fn] : listeners_)
        fn(*this, name, old_val, new_val);
}

std::string StateTree::to_json() const {
    // Simplified JSON serialization
    std::string json = "{\"type\":\"" + type_name_ + "\"";

    if (!properties_.empty()) {
        json += ",\"properties\":{";
        bool first = true;
        for (auto& [k, v] : properties_) {
            if (!first) json += ",";
            json += "\"" + k + "\":";
            if (auto* b = std::get_if<bool>(&v)) json += *b ? "true" : "false";
            else if (auto* i = std::get_if<int64_t>(&v)) json += std::to_string(*i);
            else if (auto* d = std::get_if<double>(&v)) json += std::to_string(*d);
            else if (auto* s = std::get_if<std::string>(&v)) json += "\"" + *s + "\"";
            else json += "null";
            first = false;
        }
        json += "}";
    }

    if (!children_.empty()) {
        json += ",\"children\":[";
        for (size_t i = 0; i < children_.size(); ++i) {
            if (i > 0) json += ",";
            json += children_[i]->to_json();
        }
        json += "]";
    }

    json += "}";
    return json;
}

StateTree::Ptr StateTree::from_json(std::string_view /*json*/) {
    // Full JSON parsing would use pulp::runtime::xml or a JSON parser
    // Stub for now — returns empty tree
    return create("root");
}

StateTree::Ptr StateTree::deep_copy() const {
    auto copy = create(type_name_);
    copy->properties_ = properties_;
    for (auto& child : children_)
        copy->add_child(child->deep_copy());
    return copy;
}

}  // namespace pulp::state
