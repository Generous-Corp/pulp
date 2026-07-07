#include <pulp/state/state_tree_sync.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>

namespace pulp::state {

namespace {

constexpr size_t kMaxSyncSequenceItems = std::numeric_limits<uint16_t>::max();

// Compute the dot-separated child-index path from `root` to `node`.
// Returns "" when node == root. Walks parent pointers leaf→root, so the
// path identifies the node's position within the tree rather than a
// (non-unique) type name. Returns "" if `node` is not reachable from
// `root` — apply() then targets the root, matching prior behavior.
std::string compute_path(const StateTree* root, const StateTree* node) {
    std::vector<int> indices;
    const StateTree* cur = node;
    while (cur && cur != root) {
        StateTree* parent = cur->parent();
        if (!parent) break;
        int idx = -1;
        for (int i = 0; i < parent->child_count(); ++i) {
            if (parent->child(i).get() == cur) {
                idx = i;
                break;
            }
        }
        if (idx < 0) break;
        indices.push_back(idx);
        cur = parent;
    }
    if (cur != root) return {};  // not reachable from root

    std::string path;
    for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
        if (!path.empty()) path += '.';
        path += std::to_string(*it);
    }
    return path;
}

// Resolve a dot-separated child-index path to a node within `root`.
// Returns &root for an empty path. Returns nullptr if any path segment
// is malformed or out of bounds, so apply() can safely skip the delta.
StateTree* resolve_path(StateTree& root, const std::string& path) {
    if (path.empty()) return &root;
    StateTree* cur = &root;
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t dot = path.find('.', pos);
        size_t end = (dot == std::string::npos) ? path.size() : dot;
        if (end == pos) return nullptr;  // empty segment
        int idx = 0;
        for (size_t i = pos; i < end; ++i) {
            char c = path[i];
            if (c < '0' || c > '9') return nullptr;
            idx = idx * 10 + (c - '0');
        }
        if (idx < 0 || idx >= cur->child_count()) return nullptr;
        cur = cur->child(idx).get();
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    return cur;
}

void append_u16(std::vector<uint8_t>& buf, uint16_t value) {
    buf.push_back(static_cast<uint8_t>(value & 0xFF));
    buf.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

bool read_u16(const uint8_t* data, size_t size, size_t& pos, uint16_t& value) {
    if (pos + 2 > size) return false;
    value = static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
    pos += 2;
    return true;
}

void append_string(std::vector<uint8_t>& buf, const std::string& value) {
    auto len = static_cast<uint16_t>(std::min(value.size(), kMaxSyncSequenceItems));
    append_u16(buf, len);
    buf.insert(buf.end(), value.begin(), value.begin() + len);
}

bool read_string(const uint8_t* data, size_t size, size_t& pos, std::string& value) {
    uint16_t len = 0;
    if (!read_u16(data, size, pos, len)) return false;
    if (pos + len > size) return false;
    value.assign(reinterpret_cast<const char*>(data + pos), len);
    pos += len;
    return true;
}

void append_property_value(std::vector<uint8_t>& buf, const PropertyValue& value) {
    if (auto* str = std::get_if<std::string>(&value)) {
        buf.push_back(1);
        append_string(buf, *str);
    } else if (auto* i = std::get_if<int64_t>(&value)) {
        buf.push_back(2);
        auto bits = static_cast<uint64_t>(*i);
        for (int b = 0; b < 8; ++b)
            buf.push_back(static_cast<uint8_t>((bits >> (b * 8)) & 0xFF));
    } else if (auto* f = std::get_if<double>(&value)) {
        buf.push_back(3);
        uint8_t bytes[8];
        std::memcpy(bytes, f, 8);
        buf.insert(buf.end(), bytes, bytes + 8);
    } else if (auto* b = std::get_if<bool>(&value)) {
        buf.push_back(4);
        buf.push_back(*b ? 1 : 0);
    } else if (const auto* array = get_property_array(value)) {
        buf.push_back(5);
        auto count = static_cast<uint16_t>(std::min(array->values.size(), kMaxSyncSequenceItems));
        append_u16(buf, count);
        for (uint16_t i = 0; i < count; ++i)
            append_property_value(buf, array->values[i]);
    } else if (const auto* object = get_property_object(value)) {
        buf.push_back(6);
        auto count = static_cast<uint16_t>(std::min(object->values.size(), kMaxSyncSequenceItems));
        append_u16(buf, count);
        uint16_t written = 0;
        for (const auto& [key, member] : object->values) {
            if (written++ >= count) break;
            append_string(buf, key);
            append_property_value(buf, member);
        }
    } else {
        buf.push_back(0);
    }
}

bool read_property_value(const uint8_t* data, size_t size, size_t& pos, PropertyValue& value) {
    if (pos >= size) return false;
    uint8_t val_type = data[pos++];
    if (val_type == 0) {
        value = PropertyValue{};
        return true;
    }
    if (val_type == 1) {
        std::string s;
        if (!read_string(data, size, pos, s)) return false;
        value = std::move(s);
        return true;
    }
    if (val_type == 2) {
        if (pos + 8 > size) return false;
        uint64_t bits = 0;
        for (int b = 0; b < 8; ++b)
            bits |= static_cast<uint64_t>(data[pos++]) << (b * 8);
        value = static_cast<int64_t>(bits);
        return true;
    }
    if (val_type == 3) {
        if (pos + 8 > size) return false;
        double v;
        std::memcpy(&v, data + pos, 8);
        pos += 8;
        value = v;
        return true;
    }
    if (val_type == 4) {
        if (pos >= size) return false;
        value = data[pos++] != 0;
        return true;
    }
    if (val_type == 5) {
        uint16_t count = 0;
        if (!read_u16(data, size, pos, count)) return false;
        std::vector<PropertyValue> values;
        values.reserve(count);
        for (uint16_t i = 0; i < count; ++i) {
            PropertyValue element;
            if (!read_property_value(data, size, pos, element)) return false;
            values.push_back(std::move(element));
        }
        value = make_property_array(std::move(values));
        return true;
    }
    if (val_type == 6) {
        uint16_t count = 0;
        if (!read_u16(data, size, pos, count)) return false;
        std::map<std::string, PropertyValue, std::less<>> values;
        for (uint16_t i = 0; i < count; ++i) {
            std::string key;
            PropertyValue member;
            if (!read_string(data, size, pos, key)) return false;
            if (!read_property_value(data, size, pos, member)) return false;
            values.emplace(std::move(key), std::move(member));
        }
        value = make_property_object(std::move(values));
        return true;
    }

    // Unknown future value tags cannot be skipped safely without a length field.
    // Reject the delta rather than applying a misleading null value.
    return false;
}

bool is_valid_delta_type(SyncDeltaType type) {
    switch (type) {
        case SyncDeltaType::PropertySet:
        case SyncDeltaType::PropertyRemove:
        case SyncDeltaType::ChildAdd:
        case SyncDeltaType::ChildRemove:
            return true;
    }
    return false;
}

}  // namespace

void StateTreeSynchroniser::attach_node(StateTree& node) {
    int prop_id = node.add_listener(
        [this](StateTree& n, std::string_view prop, const PropertyValue&, const PropertyValue& new_val) {
            SyncDelta delta;
            delta.type = std::holds_alternative<std::monostate>(new_val) && !n.has(prop)
                ? SyncDeltaType::PropertyRemove
                : SyncDeltaType::PropertySet;
            delta.path = compute_path(tree_.get(), &n);
            delta.key = std::string(prop);
            delta.value = new_val;
            pending_.push_back(std::move(delta));
        });
    property_listener_ids_.push_back({&node, prop_id});

    int added_id = node.add_child_added_listener(
        [this](StateTree& parent, StateTree& child, int index) {
            SyncDelta delta;
            delta.type = SyncDeltaType::ChildAdd;
            delta.path = compute_path(tree_.get(), &parent);
            delta.key = child.type_name();
            delta.child_index = index;
            pending_.push_back(std::move(delta));
            // Newly added subtrees must also be observed so their own
            // nested mutations produce deltas.
            attach_subtree(child);
        });
    child_added_listener_ids_.push_back({&node, added_id});

    int removed_id = node.add_child_removed_listener(
        [this](StateTree& parent, StateTree& child, int index) {
            SyncDelta delta;
            delta.type = SyncDeltaType::ChildRemove;
            delta.path = compute_path(tree_.get(), &parent);
            delta.child_index = index;
            pending_.push_back(std::move(delta));
            // The removed subtree's nodes may be destroyed once this
            // callback returns. Drop their listeners now so a later
            // detach() never dereferences a freed StateTree*, and a
            // re-add of a surviving child does not stack duplicates.
            detach_subtree(child);
        });
    child_removed_listener_ids_.push_back({&node, removed_id});
}

void StateTreeSynchroniser::detach_subtree(StateTree& node) {
    // Clear descendants first so the whole removed subtree is purged.
    for (int i = 0; i < node.child_count(); ++i) {
        if (auto child = node.child(i)) detach_subtree(*child);
    }
    // Remove this node's listeners and erase its id-table entries, so
    // detach() does not later call remove_*listener() through a pointer
    // to a freed StateTree.
    for (auto it = property_listener_ids_.begin(); it != property_listener_ids_.end();) {
        if (it->first == &node) {
            node.remove_listener(it->second);
            it = property_listener_ids_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = child_added_listener_ids_.begin(); it != child_added_listener_ids_.end();) {
        if (it->first == &node) {
            node.remove_child_added_listener(it->second);
            it = child_added_listener_ids_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = child_removed_listener_ids_.begin(); it != child_removed_listener_ids_.end();) {
        if (it->first == &node) {
            node.remove_child_removed_listener(it->second);
            it = child_removed_listener_ids_.erase(it);
        } else {
            ++it;
        }
    }
}

void StateTreeSynchroniser::attach_subtree(StateTree& node) {
    attach_node(node);
    for (int i = 0; i < node.child_count(); ++i) {
        if (auto child = node.child(i)) attach_subtree(*child);
    }
}

void StateTreeSynchroniser::attach(StateTree::Ptr tree) {
    detach();
    tree_ = tree;
    if (!tree_) return;
    attach_subtree(*tree_);
}

void StateTreeSynchroniser::detach() {
    for (auto& [node, id] : property_listener_ids_)
        if (node) node->remove_listener(id);
    for (auto& [node, id] : child_added_listener_ids_)
        if (node) node->remove_child_added_listener(id);
    for (auto& [node, id] : child_removed_listener_ids_)
        if (node) node->remove_child_removed_listener(id);
    property_listener_ids_.clear();
    child_added_listener_ids_.clear();
    child_removed_listener_ids_.clear();
    tree_ = nullptr;
    pending_.clear();
}

std::vector<SyncDelta> StateTreeSynchroniser::take_deltas() {
    auto result = std::move(pending_);
    pending_.clear();
    return result;
}

std::vector<uint8_t> StateTreeSynchroniser::encode(const std::vector<SyncDelta>& deltas) {
    std::vector<uint8_t> buf;

    // Encoding: count + per-delta [type, path, key, child_index, recursive value].
    uint16_t count = static_cast<uint16_t>(std::min(deltas.size(), kMaxSyncSequenceItems));
    append_u16(buf, count);

    for (uint16_t i = 0; i < count; ++i) {
        const auto& d = deltas[i];
        buf.push_back(static_cast<uint8_t>(d.type));
        append_string(buf, d.path);
        append_string(buf, d.key);
        buf.push_back(static_cast<uint8_t>(d.child_index & 0xFF));
        append_property_value(buf, d.value);
    }

    return buf;
}

std::vector<SyncDelta> StateTreeSynchroniser::decode(const uint8_t* data, size_t size) {
    std::vector<SyncDelta> deltas;
    if (size < 2 || data == nullptr) return deltas;

    size_t pos = 0;
    uint16_t count = 0;
    if (!read_u16(data, size, pos, count)) return deltas;

    for (uint16_t i = 0; i < count && pos < size; ++i) {
        SyncDelta d;
        if (pos >= size) break;
        d.type = static_cast<SyncDeltaType>(data[pos++]);
        if (!is_valid_delta_type(d.type)) break;

        if (!read_string(data, size, pos, d.path)) break;
        if (!read_string(data, size, pos, d.key)) break;

        if (pos >= size) break;
        d.child_index = static_cast<int8_t>(data[pos++]);

        if (!read_property_value(data, size, pos, d.value)) break;

        deltas.push_back(std::move(d));
    }

    return deltas;
}

void StateTreeSynchroniser::apply(StateTree& tree, const std::vector<SyncDelta>& deltas) {
    for (auto& d : deltas) {
        // Route the delta to the node identified by its path. A delta
        // captured on a nested node must be applied to that node, not
        // the root. Unresolvable paths are skipped rather than misapplied.
        StateTree* target = resolve_path(tree, d.path);
        if (!target) continue;

        switch (d.type) {
            case SyncDeltaType::PropertySet:
                target->set(d.key, d.value);
                break;
            case SyncDeltaType::PropertyRemove:
                target->remove(d.key);
                break;
            case SyncDeltaType::ChildAdd: {
                auto child = StateTree::create(d.key);
                if (d.child_index >= 0 && d.child_index < target->child_count())
                    target->insert_child(d.child_index, child);
                else
                    target->add_child(child);
                break;
            }
            case SyncDeltaType::ChildRemove:
                if (d.child_index >= 0 && d.child_index < target->child_count())
                    target->remove_child(d.child_index);
                break;
        }
    }
}

}  // namespace pulp::state
