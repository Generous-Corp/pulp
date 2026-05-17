#pragma once

// StateTree — reactive hierarchical key-value store.
// A tree of named properties with typed access, change listeners, and undo integration.
// Alternative to flat StateStore for complex nested state (presets, UI layout, etc.)

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace pulp::state {

/// Property value — can hold common types
using PropertyValue = std::variant<
    std::monostate,  // null/unset
    bool,
    int64_t,
    double,
    std::string
>;

/// Listener callback — receives the tree node, property name, old value, new value
class StateTree;
using TreeListener = std::function<void(StateTree& node, std::string_view property,
                                        const PropertyValue& old_val, const PropertyValue& new_val)>;

/// Child listener — receives parent, child, index
using ChildListener = std::function<void(StateTree& parent, StateTree& child, int index)>;

/// Reactive tree node — each node has a type name, properties, and children
class StateTree : public std::enable_shared_from_this<StateTree> {
public:
    using Ptr = std::shared_ptr<StateTree>;

    /// Create a tree node with a type name
    static Ptr create(std::string type_name) {
        return Ptr(new StateTree(std::move(type_name)));
    }

    /// Type name (e.g., "preset", "oscillator", "filter")
    const std::string& type_name() const { return type_name_; }

    // ── Properties ──────────────────────────────────────────────────────

    /// Set a property value (notifies listeners)
    void set(std::string_view name, PropertyValue value);

    /// Get a property value (returns monostate if not set)
    PropertyValue get(std::string_view name) const;

    /// Typed getters with defaults
    bool get_bool(std::string_view name, bool default_val = false) const;
    int64_t get_int(std::string_view name, int64_t default_val = 0) const;
    double get_double(std::string_view name, double default_val = 0.0) const;
    std::string get_string(std::string_view name, std::string_view default_val = "") const;

    /// Whether a property exists
    bool has(std::string_view name) const;

    /// Remove a property
    void remove(std::string_view name);

    /// Get all property names
    std::vector<std::string> property_names() const;

    // ── Children ────────────────────────────────────────────────────────

    /// Add a child node (notifies child-added listeners)
    void add_child(Ptr child);

    /// Insert a child at a specific index
    void insert_child(int index, Ptr child);

    /// Remove a child by index (notifies child-removed listeners)
    void remove_child(int index);

    /// Remove a specific child
    void remove_child(StateTree* child);

    /// Number of children
    int child_count() const { return static_cast<int>(children_.size()); }

    /// Get child by index
    Ptr child(int index) const;

    /// Find first child with matching type name
    Ptr find_child(std::string_view type_name) const;

    /// Find all children with matching type name
    std::vector<Ptr> find_children(std::string_view type_name) const;

    /// Parent (may be null for root)
    StateTree* parent() const { return parent_; }

    // ── Listeners ───────────────────────────────────────────────────────

    /// Listen for property changes
    int add_listener(TreeListener listener);
    void remove_listener(int id);

    /// Listen for child added/removed
    int add_child_added_listener(ChildListener listener);
    int add_child_removed_listener(ChildListener listener);
    void remove_child_added_listener(int id);
    void remove_child_removed_listener(int id);

    // ── Serialization ───────────────────────────────────────────────────

    /// Serialize to JSON string
    std::string to_json() const;

    /// Deserialize from JSON string
    static Ptr from_json(std::string_view json);

    // ── Deep copy ───────────────────────────────────────────────────────

    Ptr deep_copy() const;

private:
    explicit StateTree(std::string type_name) : type_name_(std::move(type_name)) {}

    std::string type_name_;
    std::map<std::string, PropertyValue, std::less<>> properties_;
    std::vector<Ptr> children_;
    StateTree* parent_ = nullptr;

    struct ListenerEntry {
        int id;
        TreeListener fn;
    };
    std::vector<ListenerEntry> listeners_;
    std::vector<std::pair<int, ChildListener>> child_added_listeners_;
    std::vector<std::pair<int, ChildListener>> child_removed_listeners_;
    int next_listener_id_ = 0;

    void notify_property_changed(std::string_view name,
                                 const PropertyValue& old_val,
                                 const PropertyValue& new_val);
};

/// Observable value — wraps a single value with change notification
template<typename T>
class ObservableValue {
public:
    ObservableValue() = default;
    explicit ObservableValue(T initial) : value_(std::move(initial)) {}

    const T& get() const { return value_; }
    operator const T&() const { return value_; }

    void set(T new_value) {
        if (value_ != new_value) {
            T old = std::move(value_);
            value_ = std::move(new_value);
            for (auto& [id, fn] : listeners_) {
                (void)id;
                if (fn) fn(old, value_);
            }
        }
    }

    ObservableValue& operator=(T new_value) {
        set(std::move(new_value));
        return *this;
    }

    using ChangeListener = std::function<void(const T& old_val, const T& new_val)>;

    int add_listener(ChangeListener fn) {
        int id = next_id_++;
        listeners_.push_back({id, std::move(fn)});
        return id;
    }

    void remove_listener(int id) {
        listeners_.erase(
            std::remove_if(listeners_.begin(), listeners_.end(),
                          [id](auto& p) { return p.first == id; }),
            listeners_.end());
    }

private:
    T value_{};
    std::vector<std::pair<int, ChangeListener>> listeners_;
    int next_id_ = 0;
};

}  // namespace pulp::state
