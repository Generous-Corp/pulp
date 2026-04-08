#include <pulp/state/state_tree_sync.hpp>
#include <cstring>

namespace pulp::state {

void StateTreeSynchroniser::attach(StateTree::Ptr tree) {
    detach();
    tree_ = tree;
    if (!tree_) return;

    listener_id_ = tree_->add_listener(
        [this](StateTree&, std::string_view prop, const PropertyValue&, const PropertyValue& new_val) {
            SyncDelta delta;
            delta.type = SyncDeltaType::PropertySet;
            delta.path = tree_->type_name();
            delta.key = std::string(prop);
            delta.value = new_val;
            pending_.push_back(std::move(delta));
        });

    int added_id = tree_->add_child_added_listener(
        [this](StateTree& parent, StateTree& child, int index) {
            SyncDelta delta;
            delta.type = SyncDeltaType::ChildAdd;
            delta.path = parent.type_name();
            delta.key = child.type_name();
            delta.child_index = index;
            pending_.push_back(std::move(delta));
        });
    child_listener_ids_.push_back(added_id);

    int removed_id = tree_->add_child_removed_listener(
        [this](StateTree& parent, StateTree&, int index) {
            SyncDelta delta;
            delta.type = SyncDeltaType::ChildRemove;
            delta.path = parent.type_name();
            delta.child_index = index;
            pending_.push_back(std::move(delta));
        });
    child_listener_ids_.push_back(removed_id);
}

void StateTreeSynchroniser::detach() {
    if (tree_ && listener_id_ >= 0)
        tree_->remove_listener(listener_id_);
    listener_id_ = -1;
    child_listener_ids_.clear();
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

    // Simple encoding: count + per-delta [type, path_len, path, key_len, key, value_type, value_data]
    uint32_t count = static_cast<uint32_t>(deltas.size());
    buf.push_back(count & 0xFF);
    buf.push_back((count >> 8) & 0xFF);

    for (auto& d : deltas) {
        buf.push_back(static_cast<uint8_t>(d.type));

        // Path
        uint16_t path_len = static_cast<uint16_t>(d.path.size());
        buf.push_back(path_len & 0xFF);
        buf.push_back((path_len >> 8) & 0xFF);
        buf.insert(buf.end(), d.path.begin(), d.path.end());

        // Key
        uint16_t key_len = static_cast<uint16_t>(d.key.size());
        buf.push_back(key_len & 0xFF);
        buf.push_back((key_len >> 8) & 0xFF);
        buf.insert(buf.end(), d.key.begin(), d.key.end());

        // Child index
        buf.push_back(static_cast<uint8_t>(d.child_index & 0xFF));

        // Value type + data
        if (auto* s = std::get_if<std::string>(&d.value)) {
            buf.push_back(1);
            uint16_t len = static_cast<uint16_t>(s->size());
            buf.push_back(len & 0xFF);
            buf.push_back((len >> 8) & 0xFF);
            buf.insert(buf.end(), s->begin(), s->end());
        } else if (auto* i = std::get_if<int64_t>(&d.value)) {
            buf.push_back(2);
            for (int b = 0; b < 8; ++b)
                buf.push_back(static_cast<uint8_t>((*i >> (b * 8)) & 0xFF));
        } else if (auto* f = std::get_if<double>(&d.value)) {
            buf.push_back(3);
            uint8_t bytes[8];
            std::memcpy(bytes, f, 8);
            buf.insert(buf.end(), bytes, bytes + 8);
        } else if (auto* b = std::get_if<bool>(&d.value)) {
            buf.push_back(4);
            buf.push_back(*b ? 1 : 0);
        } else {
            buf.push_back(0);  // null/monostate
        }
    }

    return buf;
}

std::vector<SyncDelta> StateTreeSynchroniser::decode(const uint8_t* data, size_t size) {
    std::vector<SyncDelta> deltas;
    if (size < 2) return deltas;

    size_t pos = 0;
    uint32_t count = data[pos] | (data[pos + 1] << 8);
    pos += 2;

    for (uint32_t i = 0; i < count && pos < size; ++i) {
        SyncDelta d;
        if (pos >= size) break;
        d.type = static_cast<SyncDeltaType>(data[pos++]);

        // Path
        if (pos + 2 > size) break;
        uint16_t path_len = data[pos] | (data[pos + 1] << 8); pos += 2;
        if (pos + path_len > size) break;
        d.path = std::string(reinterpret_cast<const char*>(data + pos), path_len); pos += path_len;

        // Key
        if (pos + 2 > size) break;
        uint16_t key_len = data[pos] | (data[pos + 1] << 8); pos += 2;
        if (pos + key_len > size) break;
        d.key = std::string(reinterpret_cast<const char*>(data + pos), key_len); pos += key_len;

        // Child index
        if (pos >= size) break;
        d.child_index = static_cast<int8_t>(data[pos++]);

        // Value
        if (pos >= size) break;
        uint8_t val_type = data[pos++];
        if (val_type == 1) {
            if (pos + 2 > size) break;
            uint16_t len = data[pos] | (data[pos + 1] << 8); pos += 2;
            if (pos + len > size) break;
            d.value = std::string(reinterpret_cast<const char*>(data + pos), len); pos += len;
        } else if (val_type == 2) {
            int64_t v = 0;
            for (int b = 0; b < 8 && pos < size; ++b)
                v |= static_cast<int64_t>(data[pos++]) << (b * 8);
            d.value = v;
        } else if (val_type == 3) {
            double v;
            std::memcpy(&v, data + pos, 8); pos += 8;
            d.value = v;
        } else if (val_type == 4) {
            d.value = data[pos++] != 0;
        }

        deltas.push_back(std::move(d));
    }

    return deltas;
}

void StateTreeSynchroniser::apply(StateTree& tree, const std::vector<SyncDelta>& deltas) {
    for (auto& d : deltas) {
        switch (d.type) {
            case SyncDeltaType::PropertySet:
                tree.set(d.key, d.value);
                break;
            case SyncDeltaType::PropertyRemove:
                tree.remove(d.key);
                break;
            case SyncDeltaType::ChildAdd: {
                auto child = StateTree::create(d.key);
                if (d.child_index >= 0 && d.child_index < tree.child_count())
                    tree.insert_child(d.child_index, child);
                else
                    tree.add_child(child);
                break;
            }
            case SyncDeltaType::ChildRemove:
                if (d.child_index >= 0 && d.child_index < tree.child_count())
                    tree.remove_child(d.child_index);
                break;
        }
    }
}

}  // namespace pulp::state
