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

/// Reactive tree node — each node has a type name, properties, and children.
///
/// Thread-safety contract (gap-doc Phase 0 audit, 2026-05-26):
///   StateTree is **not thread-safe**. Every public method below — including
///   const accessors like get() and property_names() — touches non-atomic
///   members (`properties_`, `children_`, listener vectors) without internal
///   locking. Callers must serialise all access from a single thread, or
///   provide their own external mutex.
///
///   The intended usage model is "main-thread / UI-thread only" (audio threads
///   should use the lock-free StateStore for parameter exchange instead). The
///   `<mutex>` header is included as a forward-compatibility hook for callers
///   that wrap an external `std::scoped_lock` around a StateTree handle; the
///   class itself holds none.
///
///   Mutating a StateTree from one thread while another thread reads it is
///   a data race even when only "harmless"-looking const methods are involved
///   (e.g. `property_names()` copies a `std::map` while a writer may rehash
///   it). The TSan regression in `test_state_tree.cpp` pins this contract.
class StateTree : public std::enable_shared_from_this<StateTree> {
public:
    using Ptr = std::shared_ptr<StateTree>;

    /// Create a tree node with a type name.
    /// Thread-safe (only touches the local arguments).
    static Ptr create(std::string type_name) {
        return Ptr(new StateTree(std::move(type_name)));
    }

    /// Type name (e.g., "preset", "oscillator", "filter").
    /// NOT thread-safe — caller must serialise with any concurrent mutator
    /// of the same node. Reading `type_name_` while another thread is
    /// destroying or reassigning the node races on the `std::string` body.
    const std::string& type_name() const { return type_name_; }

    // ── Properties ──────────────────────────────────────────────────────

    /// Set a property value (notifies listeners).
    /// NOT thread-safe — caller must serialise. Touches `properties_` and
    /// invokes every registered listener inline.
    void set(std::string_view name, PropertyValue value);

    /// Get a property value (returns monostate if not set).
    /// NOT thread-safe — caller must serialise. `std::map::find` is a non-
    /// atomic read of the tree's internal nodes.
    PropertyValue get(std::string_view name) const;

    /// Typed getters with defaults.
    /// NOT thread-safe — caller must serialise. Delegate to get().
    bool get_bool(std::string_view name, bool default_val = false) const;
    int64_t get_int(std::string_view name, int64_t default_val = 0) const;
    double get_double(std::string_view name, double default_val = 0.0) const;
    std::string get_string(std::string_view name, std::string_view default_val = "") const;

    /// Whether a property exists.
    /// NOT thread-safe — caller must serialise.
    bool has(std::string_view name) const;

    /// Remove a property.
    /// NOT thread-safe — caller must serialise. Touches `properties_` and
    /// invokes property-changed listeners with the removed value.
    void remove(std::string_view name);

    /// Get all property names (snapshot copy).
    /// NOT thread-safe — caller must serialise. The snapshot is copied while
    /// holding no lock; a concurrent set() / remove() can rehash the map
    /// during iteration.
    std::vector<std::string> property_names() const;

    // ── Children ────────────────────────────────────────────────────────

    /// Add a child node (notifies child-added listeners).
    /// NOT thread-safe — caller must serialise. Mutates `children_` and the
    /// child's `parent_` back-pointer.
    void add_child(Ptr child);

    /// Insert a child at a specific index.
    /// NOT thread-safe — caller must serialise.
    void insert_child(int index, Ptr child);

    /// Remove a child by index (notifies child-removed listeners).
    /// NOT thread-safe — caller must serialise.
    void remove_child(int index);

    /// Remove a specific child.
    /// NOT thread-safe — caller must serialise.
    void remove_child(StateTree* child);

    /// Number of children.
    /// NOT thread-safe — caller must serialise. `std::vector::size` is non-
    /// atomic relative to insert/remove.
    int child_count() const { return static_cast<int>(children_.size()); }

    /// Get child by index.
    /// NOT thread-safe — caller must serialise.
    Ptr child(int index) const;

    /// Find first child with matching type name.
    /// NOT thread-safe — caller must serialise.
    Ptr find_child(std::string_view type_name) const;

    /// Find all children with matching type name.
    /// NOT thread-safe — caller must serialise.
    std::vector<Ptr> find_children(std::string_view type_name) const;

    /// Parent (may be null for root).
    /// NOT thread-safe — caller must serialise. The raw back-pointer is
    /// rewritten by add_child / insert_child / remove_child.
    StateTree* parent() const { return parent_; }

    // ── Listeners ───────────────────────────────────────────────────────

    /// Listen for property changes.
    /// NOT thread-safe — caller must serialise. Listener vectors are not
    /// guarded; registering or removing while a notify is in flight is a
    /// data race.
    int add_listener(TreeListener listener);
    void remove_listener(int id);

    /// Listen for child added/removed.
    /// NOT thread-safe — caller must serialise.
    int add_child_added_listener(ChildListener listener);
    int add_child_removed_listener(ChildListener listener);
    void remove_child_added_listener(int id);
    void remove_child_removed_listener(int id);

    // ── Serialization ───────────────────────────────────────────────────

    /// Serialize to JSON string.
    /// NOT thread-safe — caller must serialise. Walks every property and
    /// child of the subtree.
    std::string to_json() const;

    /// Deserialize from JSON string. Returns a freshly created tree, so the
    /// call itself is thread-safe; the returned StateTree inherits the same
    /// "caller serialises" contract for subsequent access.
    /// Thread-safe (returns a new instance owned by the caller).
    static Ptr from_json(std::string_view json);

    // ── Deep copy ───────────────────────────────────────────────────────

    /// Deep-copy this subtree (including properties and recursive children).
    /// NOT thread-safe — caller must serialise with any concurrent mutator
    /// of *this. The returned copy is itself a fresh tree.
    Ptr deep_copy() const;

    // ── Synchronized clone (single-process) ─────────────────────────────

    /// Returns a deep clone of this subtree, with one-way listener
    /// wiring so that subsequent mutations on **this** mirror onto the
    /// clone within the same process. The returned `SyncedClone`
    /// (declared below) owns the listener subscriptions; destroying it
    /// detaches them, but the cloned tree itself stays usable as a
    /// snapshot.
    ///
    /// Closes the gap-doc Phase 3 row "Single-process StateTree deep
    /// clone w/ listener wiring (currently IPC-only
    /// StateTreeSynchroniser)". Mirrors:
    ///   - `set(key, v)` and `remove(key)` (typed property mutations)
    ///   - `add_child(c)` (appended children are themselves deep-copied
    ///     AND wired recursively, so deep mutations propagate)
    ///   - `remove_child(idx)` and `remove_child(StateTree*)`
    ///
    /// Mutations on the clone are NOT mirrored back to the original
    /// (one-way observer). Children added after the clone is created
    /// auto-get their own wiring. Caller still owns thread-serialisation
    /// of the original — see the class header's thread-safety contract.
    ///
    /// NOT thread-safe — caller must serialise with any concurrent
    /// mutator of *this during the clone call itself.
    class SyncedClone;
    SyncedClone clone_synced();

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

/// Owns the listener wiring created by `StateTree::clone_synced()`.
/// One instance per clone; destroy to detach.
///
/// Move-only: copying would silently double up listener registrations.
class StateTree::SyncedClone {
public:
    SyncedClone(SyncedClone&&) noexcept;
    SyncedClone& operator=(SyncedClone&&) noexcept;
    SyncedClone(const SyncedClone&) = delete;
    SyncedClone& operator=(const SyncedClone&) = delete;
    ~SyncedClone();

    /// The cloned tree. Mutate the ORIGINAL (the tree passed to
    /// `clone_synced`) to drive changes; this handle observes them.
    const StateTree::Ptr& clone() const { return clone_; }

    /// Drop the listener subscriptions early. After this returns the
    /// clone stops mirroring further mutations. Safe to call twice.
    void detach();

    /// Whether the subscription is still active.
    bool is_attached() const { return attached_; }

private:
    friend class StateTree;

    SyncedClone(StateTree::Ptr source, StateTree::Ptr cloned);

    void attach_recursive(StateTree& src, StateTree& dst);

    // Weak handle to the source node. When the source subtree is
    // removed via `remove_child` and no other shared_ptr keeps it
    // alive, `lock()` returns null so `detach()` skips deregistration
    // safely instead of dereferencing a dangling raw pointer.
    // Pinned by the SyncedClone destructor-safety regression test.
    struct WiringEntry {
        std::weak_ptr<StateTree> source;
        int prop_listener_id;
        int child_added_listener_id;
        int child_removed_listener_id;
    };

    StateTree::Ptr source_;
    StateTree::Ptr clone_;
    std::vector<WiringEntry> wiring_;
    bool attached_ = true;
};

/// Observable value — wraps a single value with change notification.
///
/// Thread-safety contract: **NOT thread-safe**. Every method (get, set,
/// add_listener, remove_listener) touches non-atomic members without
/// internal locking. Caller must serialise. Same model as StateTree.
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
