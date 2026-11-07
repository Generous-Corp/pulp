#pragma once

// StateTree — reactive hierarchical key-value store.
// A tree of named properties with typed access, change listeners, RAII
// transactions (coalesced notifications, abort, nesting) and undo recording:
// a transaction begun with an UndoManager becomes one named undo/redo step.
// Alternative to flat StateStore for complex nested state (presets, UI layout, etc.)

#include <algorithm>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <pulp/runtime/result.hpp>

namespace pulp::state {

/// Recursive property containers. These intentionally hold values, not
/// StateTree node references: embedding nodes as property values would create
/// ambiguous ownership, hidden aliasing, and potential cycles with parent/child
/// links. Represent tree shape with StateTree children; use PropertyArray /
/// PropertyObject for structured leaf data imported from a foreign state blob
/// (a host or framework's own dynamic-value / property-bag representation).
struct PropertyArray;
struct PropertyObject;

struct PropertyArrayValue {
    std::shared_ptr<const PropertyArray> storage;

    PropertyArrayValue() = default;
    explicit PropertyArrayValue(std::shared_ptr<const PropertyArray> s)
        : storage(std::move(s)) {}

    const PropertyArray* get() const noexcept { return storage.get(); }
    explicit operator bool() const noexcept { return static_cast<bool>(storage); }
};

struct PropertyObjectValue {
    std::shared_ptr<const PropertyObject> storage;

    PropertyObjectValue() = default;
    explicit PropertyObjectValue(std::shared_ptr<const PropertyObject> s)
        : storage(std::move(s)) {}

    const PropertyObject* get() const noexcept { return storage.get(); }
    explicit operator bool() const noexcept { return static_cast<bool>(storage); }
};

bool operator==(const PropertyArrayValue& a, const PropertyArrayValue& b);
bool operator!=(const PropertyArrayValue& a, const PropertyArrayValue& b);
bool operator==(const PropertyObjectValue& a, const PropertyObjectValue& b);
bool operator!=(const PropertyObjectValue& a, const PropertyObjectValue& b);

/// Property value — can hold scalar values plus immutable recursive arrays and
/// string-keyed objects. Arrays/objects use immutable shared storage only to
/// make the recursive variant possible; StateTree::set() deep-copies them on
/// ingress so callers do not create mutable aliases inside the tree.
using PropertyValue = std::variant<
    std::monostate,  // null/unset
    bool,
    int64_t,
    double,
    std::string,
    PropertyArrayValue,
    PropertyObjectValue
>;

struct PropertyArray {
    std::vector<PropertyValue> values;

    PropertyArray() = default;
    explicit PropertyArray(std::vector<PropertyValue> v) : values(std::move(v)) {}
    PropertyArray(std::initializer_list<PropertyValue> v) : values(v) {}
};

struct PropertyObject {
    using Member = std::pair<std::string, PropertyValue>;

    std::map<std::string, PropertyValue, std::less<>> values;

    PropertyObject() = default;
    explicit PropertyObject(std::map<std::string, PropertyValue, std::less<>> v)
        : values(std::move(v)) {}
    PropertyObject(std::initializer_list<Member> v) {
        for (const auto& [key, value] : v)
            values.emplace(key, value);
    }
};

inline bool operator==(const PropertyArray& a, const PropertyArray& b) {
    return a.values == b.values;
}

inline bool operator!=(const PropertyArray& a, const PropertyArray& b) {
    return !(a == b);
}

inline bool operator==(const PropertyObject& a, const PropertyObject& b) {
    return a.values == b.values;
}

inline bool operator!=(const PropertyObject& a, const PropertyObject& b) {
    return !(a == b);
}

inline bool operator==(const PropertyArrayValue& a, const PropertyArrayValue& b) {
    if (a.storage == b.storage) return true;
    if (!a.storage || !b.storage) return false;
    return *a.storage == *b.storage;
}

inline bool operator!=(const PropertyArrayValue& a, const PropertyArrayValue& b) {
    return !(a == b);
}

inline bool operator==(const PropertyObjectValue& a, const PropertyObjectValue& b) {
    if (a.storage == b.storage) return true;
    if (!a.storage || !b.storage) return false;
    return *a.storage == *b.storage;
}

inline bool operator!=(const PropertyObjectValue& a, const PropertyObjectValue& b) {
    return !(a == b);
}

inline PropertyValue make_property_array(std::vector<PropertyValue> values) {
    return PropertyArrayValue{
        std::make_shared<const PropertyArray>(PropertyArray{std::move(values)})};
}

inline PropertyValue make_property_array(std::initializer_list<PropertyValue> values) {
    return PropertyArrayValue{
        std::make_shared<const PropertyArray>(PropertyArray{values})};
}

inline PropertyValue make_property_object(std::map<std::string, PropertyValue, std::less<>> values) {
    return PropertyObjectValue{
        std::make_shared<const PropertyObject>(PropertyObject{std::move(values)})};
}

inline PropertyValue make_property_object(
    std::initializer_list<PropertyObject::Member> values) {
    return PropertyObjectValue{
        std::make_shared<const PropertyObject>(PropertyObject{values})};
}

inline const PropertyArray* get_property_array(const PropertyValue& value) {
    if (auto* ptr = std::get_if<PropertyArrayValue>(&value))
        return ptr->get();
    return nullptr;
}

inline const PropertyObject* get_property_object(const PropertyValue& value) {
    if (auto* ptr = std::get_if<PropertyObjectValue>(&value))
        return ptr->get();
    return nullptr;
}

class StateTree;
class UndoManager;

/// Listener callback — receives the tree node, property name, old value, new value
using TreeListener = std::function<void(StateTree& node, std::string_view property,
                                        const PropertyValue& old_val, const PropertyValue& new_val)>;

/// Child listener — receives parent, child, index. The index is the child's
/// position at the time of the edit; for a removal it is the index the child
/// held before it was removed.
using ChildListener = std::function<void(StateTree& parent, StateTree& child, int index)>;

/// Reorder listener — a child moved within the same parent from @p old_index
/// to @p new_index (both positions at the time of the edit).
using ChildMovedListener =
    std::function<void(StateTree& parent, StateTree& child, int old_index, int new_index)>;

/// Parent-changed listener — @p node's parent pointer changed. @p old_parent
/// is the previous parent (null if the node was a root); it is only valid for
/// the duration of the callback. Read `node.parent()` for the new parent.
using ParentChangedListener = std::function<void(StateTree& node, StateTree* old_parent)>;

/// Ancestor-changed listener — some ancestor link above @p node changed
/// (the node itself or one of its ancestors was added, removed or moved).
using AncestorChangedListener = std::function<void(StateTree& node)>;

/// When a listener is told about edits made inside a StateTree::Transaction.
///
/// - kCoalesced (default): notifications are held while a transaction is
///   open and delivered as one burst when the outermost transaction commits.
///   An aborted transaction delivers nothing. Outside a transaction the
///   listener fires inline after each edit, exactly as before.
/// - kImmediate: the listener fires inline after every edit, including edits
///   inside a transaction and the inverse edits an abort applies. Use it for
///   observers that must replay every step in order (mirrors, IPC deltas).
///   An immediate listener must not edit the tree that is notifying it.
enum class ListenerDelivery {
    kCoalesced,
    kImmediate,
};

/// Why a checked StateTree edit was refused. The tree is left unchanged and
/// no listener fires when an edit is refused.
enum class TreeEditErrorCode {
    kInvalidName,          ///< Empty property name.
    kMissingProperty,      ///< remove of a property that does not exist.
    kNullChild,            ///< Child pointer was null.
    kIndexOutOfRange,      ///< Child index outside the valid range.
    kWouldCreateCycle,     ///< Edit would make a node its own descendant.
    kTransactionConflict,  ///< Edit spans two trees with different open transactions.
    kRejectedByValidator,  ///< A PropertyValidator refused the value.
    kNotInTransaction,     ///< Node does not belong to this transaction's tree.
    kReentrantTransaction, ///< Transaction was begun from a listener during a commit burst.
    kTransactionClosed,    ///< Transaction already committed or aborted.
};

/// Error value for a refused StateTree edit.
struct TreeEditError {
    TreeEditErrorCode code;
    std::string message;
};

/// Result of a checked StateTree edit: success carries no value.
using TreeEditResult = runtime::Result<std::monostate, TreeEditError>;

/// Property validator used by checked edits. Return an error message to
/// refuse the value, or std::nullopt to accept it.
using PropertyValidator = std::function<std::optional<std::string>(
    const StateTree& node, std::string_view name, const PropertyValue& value)>;

/// Reactive tree node — each node has a type name, properties, and children.
///
/// Thread-safety contract:
///   StateTree is **not thread-safe**. Every public method below — including
///   const accessors like get() and property_names() — touches non-atomic
///   members (`properties_`, `children_`, listener vectors) without internal
///   locking. Callers must serialize all access from a single thread, or
///   provide their own external mutex.
///
///   The intended usage model is "main-thread / UI-thread only" (audio threads
///   should use the lock-free StateStore for parameter exchange instead). The
///   `<mutex>` header is included as a forward-compatibility hook for callers
///   that wrap an external `std::scoped_lock` around a StateTree handle; the
///   class itself holds none. Transactions follow the same rule: begin,
///   commit and abort on the thread that owns the tree.
///
///   Mutating a StateTree from one thread while another thread reads it is
///   a data race even when only "harmless"-looking const methods are involved
///   (e.g. `property_names()` copies a `std::map` while a writer may rehash
///   it). The TSan regression in `test_state_tree.cpp` pins this contract.
///
/// Listener contract:
///   - Listeners run after the edit is applied, so they observe the final
///     state of that edit: during a move, the `child_removed` callback on the
///     old parent already sees `child.parent()` as the new parent.
///   - A move to a different parent notifies `child_removed` on the old
///     parent, then `child_added` on the new parent, then `parent_changed` on
///     the child, then `ancestor_changed` on the child and every descendant.
///     A reorder within one parent via move_child() notifies `child_moved`.
///   - remove_all_children() detaches every child first, then notifies from
///     the last child backwards, so each `child_removed` index is valid
///     against a list that is shrinking from the end.
///   - Inside a transaction, coalesced listeners receive one burst on commit.
///     The burst preserves edit order, merges repeated changes to the same
///     property of the same node into one notification (first old value,
///     last new value) and drops property changes whose net effect is nil.
///     Structural notifications are delivered one per edit, with indices as
///     they were when the edit was made, while the tree is already in its
///     final state.
///
/// Transactions and reentrancy:
///   - begin_transaction() opens a transaction on the topmost ancestor of the
///     node, so it covers edits made through any node of that tree. A
///     transaction begun while one is already open on the tree joins it; the
///     burst is delivered when the last open Transaction ends.
///   - Edits are applied immediately; abort() reverts the edits made since
///     that Transaction began, in reverse order, and drops their pending
///     notifications. A subtree removed during the transaction stays part of
///     it until it is re-attached elsewhere, so abort can restore it.
///   - A listener running inside a commit burst may edit the tree directly
///     (those edits notify inline) but must not begin a transaction on the
///     same tree: such a Transaction is inert (is_active() is false, edits
///     are not grouped and abort() does nothing), and a ValidatedTransaction
///     begun there refuses every edit with kReentrantTransaction.
///   - Do not call UndoManager::undo() / redo() for a tree step while a
///     transaction is open on that tree; the replayed edits would join it.
class StateTree : public std::enable_shared_from_this<StateTree> {
public:
    using Ptr = std::shared_ptr<StateTree>;

    class Transaction;
    class ValidatedTransaction;

    /// Create a tree node with a type name.
    /// Thread-safe (only touches the local arguments).
    static Ptr create(std::string type_name) {
        return Ptr(new StateTree(std::move(type_name)));
    }

    /// Type name (e.g., "preset", "oscillator", "filter").
    /// NOT thread-safe — caller must serialize with any concurrent mutator
    /// of the same node. Reading `type_name_` while another thread is
    /// destroying or reassigning the node races on the `std::string` body.
    const std::string& type_name() const { return type_name_; }

    // ── Properties ──────────────────────────────────────────────────────

    /// Set a property value (notifies listeners).
    /// NOT thread-safe — caller must serialize. Touches `properties_` and
    /// invokes every registered listener inline (or queues the coalesced
    /// ones when a transaction is open).
    void set(std::string_view name, PropertyValue value);

    /// Set a property, returning an error value instead of applying an
    /// invalid edit: an empty name, or a value @p validator refuses. Intended
    /// for edits that originate from scripts or remote tools.
    /// NOT thread-safe — message thread only.
    TreeEditResult set_checked(std::string_view name, PropertyValue value,
                               const PropertyValidator& validator = {});

    /// Get a property value (returns monostate if not set).
    /// NOT thread-safe — caller must serialize. `std::map::find` is a non-
    /// atomic read of the tree's internal nodes.
    PropertyValue get(std::string_view name) const;

    /// Typed getters with defaults.
    /// NOT thread-safe — caller must serialize. Delegate to get().
    bool get_bool(std::string_view name, bool default_val = false) const;
    int64_t get_int(std::string_view name, int64_t default_val = 0) const;
    double get_double(std::string_view name, double default_val = 0.0) const;
    std::string get_string(std::string_view name, std::string_view default_val = "") const;

    /// Whether a property exists.
    /// NOT thread-safe — caller must serialize.
    bool has(std::string_view name) const;

    /// Remove a property.
    /// NOT thread-safe — caller must serialize. Touches `properties_` and
    /// invokes property-changed listeners with the removed value.
    void remove(std::string_view name);

    /// Remove a property; kMissingProperty if it does not exist.
    /// NOT thread-safe — message thread only.
    TreeEditResult remove_checked(std::string_view name);

    /// Get all property names (snapshot copy).
    /// NOT thread-safe — caller must serialize. The snapshot is copied while
    /// holding no lock; a concurrent set() / remove() can rehash the map
    /// during iteration.
    std::vector<std::string> property_names() const;

    // ── Children ────────────────────────────────────────────────────────

    /// Add a child node (notifies child-added listeners). A child that
    /// already has a parent is moved here (child_removed on the old parent,
    /// then child_added here, then parent_changed). Edits that would create
    /// a cycle, or span two trees with different open transactions, are
    /// ignored; use add_child_checked() to get the reason.
    /// NOT thread-safe — caller must serialize. Mutates `children_` and the
    /// child's `parent_` back-pointer.
    void add_child(Ptr child);

    /// Checked add_child(): returns an error instead of ignoring the edit.
    /// NOT thread-safe — message thread only.
    TreeEditResult add_child_checked(Ptr child);

    /// Insert a child at a specific index (clamped to the valid range).
    /// NOT thread-safe — caller must serialize.
    void insert_child(int index, Ptr child);

    /// Checked insert_child(): an out-of-range index is an error, not clamped.
    /// NOT thread-safe — message thread only.
    TreeEditResult insert_child_checked(int index, Ptr child);

    /// Remove a child by index (notifies child-removed listeners).
    /// NOT thread-safe — caller must serialize.
    void remove_child(int index);

    /// Checked remove_child(int).
    /// NOT thread-safe — message thread only.
    TreeEditResult remove_child_checked(int index);

    /// Remove a specific child.
    /// NOT thread-safe — caller must serialize.
    void remove_child(StateTree* child);

    /// Remove every child. All children are detached first, then
    /// child_removed / parent_changed fire from the last child backwards.
    /// NOT thread-safe — caller must serialize.
    void remove_all_children();

    /// Move the child at @p from_index of @p from_parent to @p to_index of
    /// @p to_parent. @p to_index is the child's final position: for a reorder
    /// within one parent it indexes the list after the child is taken out.
    /// Same parent: notifies child_moved. Different parent: notifies
    /// child_removed, child_added, parent_changed and ancestor_changed.
    /// Returns false (and changes nothing) if the move is invalid.
    /// NOT thread-safe — message thread only; both parents must be owned by
    /// the calling thread.
    static bool move_child(StateTree& from_parent, int from_index, StateTree& to_parent,
                           int to_index);

    /// Checked move_child().
    /// NOT thread-safe — message thread only.
    static TreeEditResult move_child_checked(StateTree& from_parent, int from_index,
                                             StateTree& to_parent, int to_index);

    /// Number of children.
    /// NOT thread-safe — caller must serialize. `std::vector::size` is non-
    /// atomic relative to insert/remove.
    int child_count() const { return static_cast<int>(children_.size()); }

    /// Get child by index.
    /// NOT thread-safe — caller must serialize.
    Ptr child(int index) const;

    /// Index of @p node among this node's children, or -1.
    /// NOT thread-safe — caller must serialize.
    int index_of(const StateTree* node) const;

    /// Find first child with matching type name.
    /// NOT thread-safe — caller must serialize.
    Ptr find_child(std::string_view type_name) const;

    /// Find all children with matching type name.
    /// NOT thread-safe — caller must serialize.
    std::vector<Ptr> find_children(std::string_view type_name) const;

    /// Visit every descendant (not this node) depth-first, parents before
    /// children. A node's child list is snapshotted right after @p fn has
    /// visited that node, so @p fn may edit the tree: children it removes
    /// from the node being visited are skipped, siblings already
    /// snapshotted are still visited even if removed later.
    /// NOT thread-safe — message thread only.
    void for_each_descendant(const std::function<void(StateTree&)>& fn);

    /// First descendant (depth-first, parents before children, not this
    /// node) for which @p predicate returns true, or null.
    /// NOT thread-safe — message thread only.
    Ptr find_descendant(const std::function<bool(const StateTree&)>& predicate) const;

    /// Structural equality: same type name, equal properties (exact variant
    /// match, so int64 3 differs from double 3.0) and equivalent children in
    /// the same order. Listeners and transaction state are not compared.
    /// NOT thread-safe — caller must serialize with mutators of both trees.
    bool is_equivalent_to(const StateTree& other) const;

    /// Parent (may be null for root).
    /// NOT thread-safe — caller must serialize. The raw back-pointer is
    /// rewritten by add_child / insert_child / remove_child / move_child.
    StateTree* parent() const { return parent_; }

    // ── Transactions ────────────────────────────────────────────────────

    /// Open (or join) a transaction on this node's tree. See the class
    /// comment for the full contract.
    /// NOT thread-safe — message thread only.
    [[nodiscard]] Transaction begin_transaction();

    /// Open (or join) a transaction that records inverse edits so the whole
    /// transaction becomes one named step in @p undo when the outermost
    /// transaction commits. The first manager supplied while the transaction
    /// is open wins; a later one passed by a joining transaction is ignored.
    /// An aborted or empty transaction pushes no step.
    /// NOT thread-safe — message thread only; @p undo must outlive the commit.
    [[nodiscard]] Transaction begin_transaction(UndoManager* undo, std::string_view name);

    /// Open (or join) a transaction whose edits are checked and return
    /// TreeEditResult. @p validator (optional) vets every property value.
    /// NOT thread-safe — message thread only.
    [[nodiscard]] ValidatedTransaction begin_validated_transaction(PropertyValidator validator = {},
                                                                   UndoManager* undo = nullptr,
                                                                   std::string_view name = {});

    /// Whether a transaction is open that covers edits made through this node.
    /// NOT thread-safe — caller must serialize.
    bool in_transaction() const;

    // ── Listeners ───────────────────────────────────────────────────────

    /// Listen for property changes.
    /// NOT thread-safe — caller must serialize. Listener vectors are not
    /// guarded; registering or removing while a notify is in flight on
    /// another thread is a data race.
    int add_listener(TreeListener listener,
                     ListenerDelivery delivery = ListenerDelivery::kCoalesced);
    void remove_listener(int id);

    /// Listen for child added/removed.
    /// NOT thread-safe — caller must serialize.
    int add_child_added_listener(ChildListener listener,
                                 ListenerDelivery delivery = ListenerDelivery::kCoalesced);
    int add_child_removed_listener(ChildListener listener,
                                   ListenerDelivery delivery = ListenerDelivery::kCoalesced);
    void remove_child_added_listener(int id);
    void remove_child_removed_listener(int id);

    /// Listen for reorders within this node (move_child with one parent).
    /// NOT thread-safe — caller must serialize.
    int add_child_moved_listener(ChildMovedListener listener,
                                 ListenerDelivery delivery = ListenerDelivery::kCoalesced);
    void remove_child_moved_listener(int id);

    /// Listen for this node's parent changing.
    /// NOT thread-safe — caller must serialize.
    int add_parent_changed_listener(ParentChangedListener listener,
                                    ListenerDelivery delivery = ListenerDelivery::kCoalesced);
    void remove_parent_changed_listener(int id);

    /// Listen for any ancestor link above this node changing.
    /// NOT thread-safe — caller must serialize.
    int add_ancestor_changed_listener(AncestorChangedListener listener,
                                      ListenerDelivery delivery = ListenerDelivery::kCoalesced);
    void remove_ancestor_changed_listener(int id);

    // ── Serialization ───────────────────────────────────────────────────

    /// Serialize to JSON string.
    /// NOT thread-safe — caller must serialize. Walks every property and
    /// child of the subtree.
    std::string to_json() const;

    /// Deserialize from JSON string. Returns a freshly created tree, so the
    /// call itself is thread-safe; the returned StateTree inherits the same
    /// "caller serializes" contract for subsequent access.
    /// Thread-safe (returns a new instance owned by the caller).
    static Ptr from_json(std::string_view json);

    // ── Deep copy ───────────────────────────────────────────────────────

    /// Deep-copy this subtree (including properties and recursive children).
    /// NOT thread-safe — caller must serialize with any concurrent mutator
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
    /// Mirrors mutations from this tree into the same-process clone:
    ///   - `set(key, v)` and `remove(key)` (typed property mutations)
    ///   - `add_child(c)` (appended children are themselves deep-copied
    ///     AND wired recursively, so deep mutations propagate)
    ///   - `remove_child(idx)` and `remove_child(StateTree*)`
    ///   - reorders made with `move_child()`
    ///
    /// The wiring uses immediate delivery, so the clone tracks every edit
    /// inside a transaction (and every revert of an aborted one) in order.
    ///
    /// Mutations on the clone are NOT mirrored back to the original
    /// (one-way observer). Children added after the clone is created
    /// auto-get their own wiring. Caller still owns thread-serialization
    /// of the original — see the class header's thread-safety contract.
    ///
    /// NOT thread-safe — caller must serialize with any concurrent
    /// mutator of *this during the clone call itself.
    class SyncedClone;
    SyncedClone clone_synced();

private:
    explicit StateTree(std::string type_name) : type_name_(std::move(type_name)) {}

    struct TxState;
    struct Edit;
    struct Pending;
    enum class Pass { kAll, kImmediate, kCoalesced };

    template <typename Fn> struct Slot {
        int id;
        Fn fn;
        ListenerDelivery delivery;
    };

    std::string type_name_;
    std::map<std::string, PropertyValue, std::less<>> properties_;
    std::vector<Ptr> children_;
    StateTree* parent_ = nullptr;

    std::vector<Slot<TreeListener>> listeners_;
    std::vector<Slot<ChildListener>> child_added_listeners_;
    std::vector<Slot<ChildListener>> child_removed_listeners_;
    std::vector<Slot<ChildMovedListener>> child_moved_listeners_;
    std::vector<Slot<ParentChangedListener>> parent_changed_listeners_;
    std::vector<Slot<AncestorChangedListener>> ancestor_changed_listeners_;
    int next_listener_id_ = 0;

    // Open transaction owned by this node (when it is the tree it was begun
    // on) or held by it (when it was detached while a transaction was open).
    std::shared_ptr<TxState> tx_;
    // Non-zero while this node's transaction is delivering its commit burst.
    int flushing_ = 0;

    template <typename Fn>
    int add_slot(std::vector<Slot<Fn>>& slots, Fn fn, ListenerDelivery delivery);
    template <typename Fn> static void remove_slot(std::vector<Slot<Fn>>& slots, int id);
    template <typename Fn, typename... Args>
    static void dispatch(std::vector<Slot<Fn>>& slots, Pass pass, Args&... args);

    std::shared_ptr<TxState> active_tx() const;
    bool flush_in_progress() const;
    static TreeEditResult check_tx(const StateTree* a, const StateTree* b);

    void apply_property(std::string_view name, std::optional<PropertyValue> value);
    static void relocate(const Ptr& child, StateTree* to, int to_index, bool reorder_event);
    static void place(const Ptr& child, StateTree* to, int to_index);
    static void replay(const Edit& edit, bool forward);
    static void replay_log(const std::vector<Edit>& log, const Ptr& owner, bool forward);
    static void emit(Pending pending, TxState* tx);
    static void deliver(const Pending& pending, Pass pass);
    static void finish(std::shared_ptr<TxState> state);

    // True if this node is `node` or one of its ancestors (walks `node`'s
    // parent chain upward). Used by add_child / insert_child / move_child to
    // reject parent/child cycles before they can form.
    bool is_ancestor_of(const StateTree* node) const;
};

/// RAII transaction on a StateTree. Edits made while it is open are applied
/// immediately; coalesced listener notifications are held until the
/// outermost transaction on the tree ends, then delivered as one burst.
/// Destroying an active Transaction commits it.
///
/// Move-only. NOT thread-safe — commit / abort on the thread that owns the
/// tree.
class StateTree::Transaction {
  public:
    /// An inert transaction (is_active() is false).
    Transaction() = default;
    Transaction(Transaction&& other) noexcept;
    /// Commits the transaction currently held, then takes over @p other.
    Transaction& operator=(Transaction&& other) noexcept;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction();

    /// End this transaction and keep its edits. When it is the last open
    /// transaction on the tree, the notification burst is delivered and the
    /// undo step (if any) is pushed. No-op when inactive.
    void commit();

    /// Revert every edit made since this transaction began, in reverse
    /// order, drop their pending notifications, and end the transaction.
    /// Returns false when inactive. Immediate listeners see the reverting
    /// edits; coalesced listeners see nothing for the reverted edits.
    bool abort();

    /// Whether this object still holds an open transaction.
    bool is_active() const {
        return active_;
    }

  private:
    friend class StateTree;
    friend class StateTree::ValidatedTransaction;
    Transaction(std::shared_ptr<TxState> state, size_t edit_mark, size_t pending_mark)
        : state_(std::move(state)), edit_mark_(edit_mark), pending_mark_(pending_mark),
          active_(true) {}
    void end();

    std::shared_ptr<TxState> state_;
    size_t edit_mark_ = 0;
    size_t pending_mark_ = 0;
    bool active_ = false;
};

/// A transaction whose edits are checked and return TreeEditResult instead
/// of being silently ignored. Every edit must target a node of the tree the
/// transaction was begun on (kNotInTransaction otherwise). A refused edit
/// changes nothing; the first refusal is remembered, and commit() then
/// aborts the whole transaction and returns that error. Destroying an
/// active ValidatedTransaction calls commit().
///
/// Move-only. NOT thread-safe — message thread only.
class StateTree::ValidatedTransaction {
  public:
    ValidatedTransaction(ValidatedTransaction&& other) noexcept = default;
    ValidatedTransaction& operator=(ValidatedTransaction&& other) noexcept;
    ValidatedTransaction(const ValidatedTransaction&) = delete;
    ValidatedTransaction& operator=(const ValidatedTransaction&) = delete;
    ~ValidatedTransaction();

    TreeEditResult set(StateTree& node, std::string_view name, PropertyValue value);
    TreeEditResult remove(StateTree& node, std::string_view name);
    TreeEditResult add_child(StateTree& parent, Ptr child);
    TreeEditResult insert_child(StateTree& parent, int index, Ptr child);
    TreeEditResult remove_child(StateTree& parent, int index);
    TreeEditResult move_child(StateTree& from_parent, int from_index, StateTree& to_parent,
                              int to_index);

    /// Commit when every edit succeeded; otherwise abort and return the
    /// first error. kTransactionClosed when already ended.
    TreeEditResult commit();

    /// Revert every edit and end the transaction. Returns false when inactive.
    bool abort();

    bool is_active() const {
        return tx_.is_active();
    }
    bool has_error() const {
        return first_error_.has_value();
    }
    const std::optional<TreeEditError>& first_error() const {
        return first_error_;
    }

  private:
    friend class StateTree;
    ValidatedTransaction(Transaction tx, PropertyValidator validator, bool reentrant)
        : tx_(std::move(tx)), validator_(std::move(validator)), reentrant_(reentrant) {}

    TreeEditResult guard(std::initializer_list<const StateTree*> nodes) const;
    TreeEditResult note(TreeEditResult result);

    Transaction tx_;
    PropertyValidator validator_;
    std::optional<TreeEditError> first_error_;
    bool reentrant_ = false;
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
    void detach_recursive(StateTree& src);

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
        int child_moved_listener_id;
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
/// internal locking. Caller must serialize. Same model as StateTree.
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
            // Snapshot-then-dispatch with a liveness recheck, mirroring
            // StateTree's listener fan-out: a callback may add or remove
            // listeners on this same value while it runs, which would
            // invalidate iterators into the live vector. A listener removed
            // earlier in the same dispatch is skipped; one added during the
            // dispatch is not invoked for the in-flight set. The single-
            // listener fast path copies the callable before invoking so a
            // self-removing sole listener can't free its own executing body.
            if (listeners_.size() == 1) {
                auto fn = listeners_.front().second;
                if (fn) fn(old, value_);
                return;
            }
            auto snapshot = listeners_;
            for (auto& [id, fn] : snapshot) {
                if (!fn) continue;
                bool still_registered = std::any_of(
                    listeners_.begin(), listeners_.end(),
                    [id = id](const auto& e) { return e.first == id; });
                if (still_registered) fn(old, value_);
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
