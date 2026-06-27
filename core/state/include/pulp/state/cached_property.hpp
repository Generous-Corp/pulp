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

    /// Set the value (updates both cache and tree).
    ///
    /// Short-circuits a redundant write-through: if the new value equals the
    /// current cached value AND the property is already materialized in the
    /// tree, the write (and its listener notification) is skipped. The
    /// present-check is essential — when the property is absent and the cache
    /// is still sitting on `default_`, a `set(default_)` MUST materialize the
    /// property, so it must not be swallowed by the value-equality test.
    void set(const T& new_value) {
        if (tree_ && value_ == new_value && tree_->has(property_name_))
            return;
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
          value_(std::move(other.value_)), default_(std::move(other.default_)) {
        // Remove old listener from the source and re-register on this object
        if (tree_ && other.listener_id_ >= 0) {
            tree_->remove_listener(other.listener_id_);
            other.listener_id_ = -1;
            listener_id_ = tree_->add_listener(
                [this](StateTree&, std::string_view prop, const PropertyValue&, const PropertyValue& new_val) {
                    if (prop == property_name_)
                        update_from_variant(new_val);
                });
        }
    }

private:
    StateTree::Ptr tree_;
    std::string property_name_;
    T value_{};
    T default_{};
    int listener_id_ = -1;

    void update_from_variant(const PropertyValue& val);
};

// Whether the incoming variant means "the property is absent" — i.e. it was
// removed from the tree (StateTree::remove notifies with an empty
// PropertyValue{}) or never existed (StateTree::get of an unknown key returns
// std::monostate). This is the ONLY signal for absence: a property that is
// still present but holds an unexpected/uncoercible type (e.g. a string where
// this cache wants a bool) is NOT absent, and the cache must keep its prior
// value rather than reverting to the default.
inline bool property_is_absent(const PropertyValue& val) {
    return std::holds_alternative<std::monostate>(val);
}

// Specializations for common types. Each reverts to `default_` when the
// property is absent (removed), applies the value on a type match (with the
// documented numeric coercion for double), and otherwise leaves the cache
// unchanged so a present-but-mismatched value does not clobber it.
template<> inline void CachedProperty<bool>::update_from_variant(const PropertyValue& val) {
    if (property_is_absent(val)) value_ = default_;
    else if (auto* b = std::get_if<bool>(&val)) value_ = *b;
}
template<> inline void CachedProperty<int64_t>::update_from_variant(const PropertyValue& val) {
    if (property_is_absent(val)) value_ = default_;
    else if (auto* i = std::get_if<int64_t>(&val)) value_ = *i;
}
template<> inline void CachedProperty<double>::update_from_variant(const PropertyValue& val) {
    if (property_is_absent(val)) value_ = default_;
    else if (auto* d = std::get_if<double>(&val)) value_ = *d;
    else if (auto* i = std::get_if<int64_t>(&val)) value_ = static_cast<double>(*i);
}
template<> inline void CachedProperty<std::string>::update_from_variant(const PropertyValue& val) {
    if (property_is_absent(val)) value_ = default_;
    else if (auto* s = std::get_if<std::string>(&val)) value_ = *s;
}

}  // namespace pulp::state
