#pragma once

// CachedProperty — typed accessor for StateTree properties with local cache.
// Avoids repeated variant extraction on every read. Updates cache on tree change.

#include <pulp/state/state_tree.hpp>
#include <string>
#include <functional>

namespace pulp::state {

/// Cached accessor for a StateTree property.
/// Maintains a local copy that's updated via the tree's listener mechanism.
template<typename T>
class CachedProperty {
public:
    CachedProperty() = default;

    /// Bind to a tree node and property name
    CachedProperty(StateTree::Ptr tree, std::string_view property_name, T default_value = T{})
        : tree_(tree), property_name_(property_name), value_(default_value), default_(default_value) {
        if (tree_) {
            refresh();
            listener_id_ = tree_->add_listener(
                [this](StateTree&, std::string_view prop, const PropertyValue&, const PropertyValue& new_val) {
                    if (prop == property_name_)
                        update_from_variant(new_val);
                });
        }
    }

    ~CachedProperty() {
        if (tree_ && listener_id_ >= 0)
            tree_->remove_listener(listener_id_);
    }

    /// Get the cached value (no variant extraction — fast)
    const T& get() const { return value_; }
    operator const T&() const { return value_; }

    /// Set the value (updates both cache and tree)
    void set(const T& new_value) {
        value_ = new_value;
        if (tree_)
            tree_->set(property_name_, PropertyValue(new_value));
    }

    CachedProperty& operator=(const T& new_value) { set(new_value); return *this; }

    /// Force refresh from the tree
    void refresh() {
        if (!tree_) return;
        auto val = tree_->get(property_name_);
        update_from_variant(val);
    }

    /// Whether bound to a tree
    bool is_bound() const { return tree_ != nullptr; }

    // Move only (listener management)
    CachedProperty(const CachedProperty&) = delete;
    CachedProperty& operator=(const CachedProperty&) = delete;
    CachedProperty(CachedProperty&& other) noexcept
        : tree_(std::move(other.tree_)), property_name_(std::move(other.property_name_)),
          value_(std::move(other.value_)), default_(std::move(other.default_)),
          listener_id_(other.listener_id_) {
        other.listener_id_ = -1;
    }

private:
    StateTree::Ptr tree_;
    std::string property_name_;
    T value_{};
    T default_{};
    int listener_id_ = -1;

    void update_from_variant(const PropertyValue& val);
};

// Specializations for common types
template<> inline void CachedProperty<bool>::update_from_variant(const PropertyValue& val) {
    if (auto* b = std::get_if<bool>(&val)) value_ = *b;
}
template<> inline void CachedProperty<int64_t>::update_from_variant(const PropertyValue& val) {
    if (auto* i = std::get_if<int64_t>(&val)) value_ = *i;
}
template<> inline void CachedProperty<double>::update_from_variant(const PropertyValue& val) {
    if (auto* d = std::get_if<double>(&val)) value_ = *d;
    else if (auto* i = std::get_if<int64_t>(&val)) value_ = static_cast<double>(*i);
}
template<> inline void CachedProperty<std::string>::update_from_variant(const PropertyValue& val) {
    if (auto* s = std::get_if<std::string>(&val)) value_ = *s;
}

}  // namespace pulp::state
