#include <pulp/state/state_tree.hpp>
#include <pulp/state/undo_manager.hpp>
#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <map>
#include <utility>

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

// ── Transaction bookkeeping ─────────────────────────────────────────────
//
// An Edit is one applied change with enough data to apply it again or
// revert it; abort() and undo/redo replay the log. A Pending is one
// notification, held for coalesced listeners until the outermost
// transaction ends. Both hold strong node references so a node removed
// during the transaction (or referenced from undo history) stays alive.

struct StateTree::Edit {
    enum class Kind { kProperty, kInsert, kRemove, kMove };
    Kind kind = Kind::kProperty;
    Ptr node;        // kProperty: the edited node; child kinds: the child
    Ptr parent;      // kInsert / kRemove: the parent; kMove: the source parent
    Ptr parent2;     // kMove: the destination parent
    int index = -1;  // kInsert / kRemove: position; kMove: source position
    int index2 = -1; // kMove: destination position
    std::string name;
    std::optional<PropertyValue> before;
    std::optional<PropertyValue> after;
};

struct StateTree::Pending {
    enum class Kind { kProperty, kChildAdded, kChildRemoved, kChildMoved, kParentChanged };
    Kind kind = Kind::kProperty;
    Ptr node;  // property node, parent for child kinds, the child for kParentChanged
    Ptr child; // child kinds
    Ptr other; // kParentChanged: the old parent
    std::string name;
    PropertyValue old_val;
    PropertyValue new_val;
    bool existed_before = false;
    int index = -1;
    int index2 = -1;
};

struct StateTree::TxState {
    Ptr owner;
    int depth = 0;
    bool open = true;
    bool reverting = false;
    UndoManager* undo = nullptr;
    std::string undo_name;
    std::vector<Edit> edits;
    std::vector<Pending> pending;
    // Nodes detached while the transaction was open; they carry `tx_` so
    // edits made through them are still recorded.
    std::vector<std::weak_ptr<StateTree>> holders;
};

namespace {

TreeEditResult edit_ok() {
    return TreeEditResult(runtime::Ok(std::monostate{}));
}

TreeEditResult edit_err(TreeEditErrorCode code, std::string message) {
    return TreeEditResult(runtime::Err(TreeEditError{code, std::move(message)}));
}

} // namespace

std::shared_ptr<StateTree::TxState> StateTree::active_tx() const {
    for (const StateTree* n = this; n != nullptr; n = n->parent_)
        if (n->tx_ && n->tx_->open)
            return n->tx_;
    return nullptr;
}

bool StateTree::flush_in_progress() const {
    for (const StateTree* n = this; n != nullptr; n = n->parent_)
        if (n->flushing_ > 0)
            return true;
    return false;
}

TreeEditResult StateTree::check_tx(const StateTree* a, const StateTree* b) {
    auto ta = a ? a->active_tx() : nullptr;
    auto tb = b ? b->active_tx() : nullptr;
    if (ta && tb && ta != tb)
        return edit_err(TreeEditErrorCode::kTransactionConflict,
                        "edit spans two trees with different open transactions");
    return edit_ok();
}

bool StateTree::in_transaction() const {
    return active_tx() != nullptr;
}

// ── Listener plumbing ───────────────────────────────────────────────────
//
// Listener fan-out is snapshot-then-dispatch with a liveness recheck. A
// callback may register or unregister listeners (or add/remove children) on
// this same node while it runs, which would invalidate iterators/references
// into the live backing vectors. We copy the entries up front, then before
// each call confirm the id is STILL registered — so a listener removed
// earlier in the same dispatch is not invoked, and a listener added during
// the dispatch is not invoked for the in-flight notification. The single-
// listener case (the common one) skips the copy but still copies the
// callable before invoking: a sole listener that removes itself mid-callback
// would otherwise free the std::function whose body is still executing.

namespace {

bool wants(ListenerDelivery delivery, int pass) {
    // pass: 0 = all, 1 = immediate only, 2 = coalesced only
    if (pass == 0)
        return true;
    return (pass == 1) == (delivery == ListenerDelivery::kImmediate);
}

} // namespace

template <typename Fn>
int StateTree::add_slot(std::vector<Slot<Fn>>& slots, Fn fn, ListenerDelivery delivery) {
    int id = next_listener_id_++;
    slots.push_back({id, std::move(fn), delivery});
    return id;
}

template <typename Fn> void StateTree::remove_slot(std::vector<Slot<Fn>>& slots, int id) {
    slots.erase(
        std::remove_if(slots.begin(), slots.end(), [id](const Slot<Fn>& s) { return s.id == id; }),
        slots.end());
}

template <typename Fn, typename... Args>
void StateTree::dispatch(std::vector<Slot<Fn>>& slots, Pass pass, Args&... args) {
    const int p = pass == Pass::kAll ? 0 : (pass == Pass::kImmediate ? 1 : 2);
    if (slots.empty())
        return;
    if (slots.size() == 1) {
        if (!wants(slots.front().delivery, p))
            return;
        auto fn = slots.front().fn;
        if (fn)
            fn(args...);
        return;
    }
    auto snapshot = slots;
    for (auto& s : snapshot) {
        if (!s.fn || !wants(s.delivery, p))
            continue;
        bool still_registered = std::any_of(slots.begin(), slots.end(),
                                            [id = s.id](const Slot<Fn>& e) { return e.id == id; });
        if (still_registered)
            s.fn(args...);
    }
}

void StateTree::deliver(const Pending& p, Pass pass) {
    switch (p.kind) {
    case Pending::Kind::kProperty: {
        std::string_view name = p.name;
        dispatch(p.node->listeners_, pass, *p.node, name, p.old_val, p.new_val);
        break;
    }
    case Pending::Kind::kChildAdded: {
        int index = p.index;
        dispatch(p.node->child_added_listeners_, pass, *p.node, *p.child, index);
        break;
    }
    case Pending::Kind::kChildRemoved: {
        int index = p.index;
        dispatch(p.node->child_removed_listeners_, pass, *p.node, *p.child, index);
        break;
    }
    case Pending::Kind::kChildMoved: {
        int from = p.index;
        int to = p.index2;
        dispatch(p.node->child_moved_listeners_, pass, *p.node, *p.child, from, to);
        break;
    }
    case Pending::Kind::kParentChanged: {
        StateTree* old_parent = p.other.get();
        dispatch(p.node->parent_changed_listeners_, pass, *p.node, old_parent);
        // Every node of the subtree saw an ancestor change. Collect the
        // interested nodes first so callbacks may edit the subtree.
        std::vector<Ptr> interested;
        std::vector<Ptr> stack{p.node};
        while (!stack.empty()) {
            auto n = std::move(stack.back());
            stack.pop_back();
            if (!n->ancestor_changed_listeners_.empty())
                interested.push_back(n);
            for (auto it = n->children_.rbegin(); it != n->children_.rend(); ++it)
                stack.push_back(*it);
        }
        for (auto& n : interested)
            dispatch(n->ancestor_changed_listeners_, pass, *n);
        break;
    }
    }
}

void StateTree::emit(Pending pending, TxState* tx) {
    if (tx == nullptr) {
        deliver(pending, Pass::kAll);
        return;
    }
    deliver(pending, Pass::kImmediate);
    if (!tx->reverting)
        tx->pending.push_back(std::move(pending));
}

// ── Core edits ──────────────────────────────────────────────────────────

void StateTree::apply_property(std::string_view name, std::optional<PropertyValue> value) {
    auto it = properties_.find(name);
    std::optional<PropertyValue> before;
    if (it != properties_.end())
        before = it->second;
    if (!value && !before)
        return;

    std::optional<PropertyValue> after;
    if (value) {
        after = clone_property_value(*value);
        if (it != properties_.end())
            it->second = *after;
        else
            properties_.emplace(std::string(name), *after);
    } else {
        properties_.erase(it);
    }

    auto tx = active_tx();
    auto self = shared_from_this();
    if (tx && !tx->reverting) {
        Edit e;
        e.kind = Edit::Kind::kProperty;
        e.node = self;
        e.name = std::string(name);
        e.before = before;
        e.after = after;
        tx->edits.push_back(std::move(e));
    }

    Pending p;
    p.kind = Pending::Kind::kProperty;
    p.node = std::move(self);
    p.name = std::string(name);
    p.old_val = before.value_or(PropertyValue{});
    p.new_val = after.value_or(PropertyValue{});
    p.existed_before = before.has_value();
    emit(std::move(p), tx.get());
}

// Moves `child` from its current parent (if any) to `to` at `to_index`, or
// detaches it when `to` is null. Callers validate first: no cycle, index in
// range, no transaction conflict. `reorder_event` selects child_moved (rather
// than removed + added) for a move within one parent.
void StateTree::relocate(const Ptr& child, StateTree* to, int to_index, bool reorder_event) {
    StateTree* from = child->parent_;
    const int from_index = from ? from->index_of(child.get()) : -1;

    auto tx = from ? from->active_tx() : child->active_tx();
    // A child that is itself the root of an open transaction (and was then
    // attached to a tree without one) still records its own detachment.
    if (!tx && child->tx_ && child->tx_->open)
        tx = child->tx_;
    if (!tx && to)
        tx = to->active_tx();

    Ptr from_ptr = from ? from->shared_from_this() : nullptr;
    Ptr to_ptr = to ? to->shared_from_this() : nullptr;

    if (from) {
        from->children_.erase(from->children_.begin() + from_index);
        child->parent_ = nullptr;
    }
    if (to) {
        to->children_.insert(to->children_.begin() + to_index, child);
        child->parent_ = to;
        // Attached again: a node that only held the transaction because it
        // had been detached hands it back to its new tree.
        if (child->tx_ && (!child->tx_->open || child->tx_->owner.get() != child.get()))
            child->tx_.reset();
    } else if (tx && !tx->reverting && !(child->tx_ && child->tx_->open)) {
        child->tx_ = tx;
        tx->holders.push_back(child);
    } else if (tx && tx->reverting && !child->tx_) {
        // A revert can detach a node that was added during the transaction;
        // keep it covered in case an outer abort needs to move it again.
        child->tx_ = tx;
        tx->holders.push_back(child);
    }

    if (tx && !tx->reverting) {
        Edit e;
        e.node = child;
        if (from && to) {
            e.kind = Edit::Kind::kMove;
            e.parent = from_ptr;
            e.index = from_index;
            e.parent2 = to_ptr;
            e.index2 = to_index;
        } else if (to) {
            e.kind = Edit::Kind::kInsert;
            e.parent = to_ptr;
            e.index = to_index;
        } else {
            e.kind = Edit::Kind::kRemove;
            e.parent = from_ptr;
            e.index = from_index;
        }
        tx->edits.push_back(std::move(e));
    }

    if (from && from == to && reorder_event) {
        Pending p;
        p.kind = Pending::Kind::kChildMoved;
        p.node = from_ptr;
        p.child = child;
        p.index = from_index;
        p.index2 = to_index;
        emit(std::move(p), tx.get());
        return;
    }
    if (from) {
        Pending p;
        p.kind = Pending::Kind::kChildRemoved;
        p.node = from_ptr;
        p.child = child;
        p.index = from_index;
        emit(std::move(p), tx.get());
    }
    if (to) {
        Pending p;
        p.kind = Pending::Kind::kChildAdded;
        p.node = to_ptr;
        p.child = child;
        p.index = to_index;
        emit(std::move(p), tx.get());
    }
    if (from != to) {
        Pending p;
        p.kind = Pending::Kind::kParentChanged;
        p.node = child;
        p.other = from_ptr;
        emit(std::move(p), tx.get());
    }
}

// Best-effort placement used by abort and undo/redo replay: the tree may
// have drifted since the edit was recorded, so indices are clamped and an
// edit that would now form a cycle is skipped.
void StateTree::place(const Ptr& child, StateTree* to, int to_index) {
    if (!child)
        return;
    if (to == nullptr) {
        if (child->parent_)
            relocate(child, nullptr, -1, false);
        return;
    }
    if (child->is_ancestor_of(to))
        return;
    const bool same = child->parent_ == to;
    const int limit = to->child_count() - (same ? 1 : 0);
    to_index = std::clamp(to_index, 0, limit);
    if (same && to->index_of(child.get()) == to_index)
        return;
    relocate(child, to, to_index, same);
}

void StateTree::replay(const Edit& e, bool forward) {
    switch (e.kind) {
    case Edit::Kind::kProperty:
        e.node->apply_property(e.name, forward ? e.after : e.before);
        break;
    case Edit::Kind::kInsert:
        if (forward)
            place(e.node, e.parent.get(), e.index);
        else if (e.node->parent_ == e.parent.get())
            place(e.node, nullptr, -1);
        break;
    case Edit::Kind::kRemove:
        if (forward) {
            if (e.node->parent_ == e.parent.get())
                place(e.node, nullptr, -1);
        } else {
            place(e.node, e.parent.get(), e.index);
        }
        break;
    case Edit::Kind::kMove:
        if (forward)
            place(e.node, e.parent2.get(), e.index2);
        else
            place(e.node, e.parent.get(), e.index);
        break;
    }
}

void StateTree::replay_log(const std::vector<Edit>& log, const Ptr& owner, bool forward) {
    // Group the replayed edits so coalesced listeners see one burst.
    auto tx = owner->begin_transaction();
    if (forward) {
        for (const auto& e : log)
            replay(e, true);
    } else {
        for (auto it = log.rbegin(); it != log.rend(); ++it)
            replay(*it, false);
    }
    tx.commit();
}

void StateTree::finish(std::shared_ptr<TxState> state) {
    state->open = false;
    Ptr owner = state->owner;
    if (owner && owner->tx_ == state)
        owner->tx_.reset();
    for (auto& weak : state->holders)
        if (auto n = weak.lock())
            if (n->tx_ == state)
                n->tx_.reset();
    state->holders.clear();

    if (state->undo && !state->edits.empty()) {
        auto log = std::make_shared<const std::vector<Edit>>(std::move(state->edits));
        state->undo->add_without_executing(UndoAction::create(
            state->undo_name.empty() ? std::string("Edit") : state->undo_name,
            [log, owner] { replay_log(*log, owner, false); },
            [log, owner] { replay_log(*log, owner, true); }));
    }
    state->edits.clear();

    // Coalesce: one notification per (node, property) at the position of its
    // first change, carrying the first old value and the last new value.
    std::vector<Pending> burst;
    burst.reserve(state->pending.size());
    std::map<std::pair<const StateTree*, std::string>, size_t> property_slot;
    for (auto& p : state->pending) {
        if (p.kind == Pending::Kind::kProperty) {
            auto key = std::make_pair(static_cast<const StateTree*>(p.node.get()), p.name);
            auto found = property_slot.find(key);
            if (found != property_slot.end()) {
                burst[found->second].new_val = std::move(p.new_val);
                continue;
            }
            property_slot.emplace(std::move(key), burst.size());
        }
        burst.push_back(std::move(p));
    }
    state->pending.clear();
    burst.erase(std::remove_if(burst.begin(), burst.end(),
                               [](const Pending& p) {
                                   return p.kind == Pending::Kind::kProperty &&
                                          p.old_val == p.new_val &&
                                          p.existed_before == p.node->has(p.name);
                               }),
                burst.end());

    if (burst.empty() || !owner)
        return;
    struct FlushGuard {
        StateTree& node;
        explicit FlushGuard(StateTree& n) : node(n) {
            ++node.flushing_;
        }
        ~FlushGuard() {
            --node.flushing_;
        }
    } guard(*owner);
    for (const auto& p : burst)
        deliver(p, Pass::kCoalesced);
}

// ── Transaction objects ─────────────────────────────────────────────────

StateTree::Transaction StateTree::begin_transaction() {
    return begin_transaction(nullptr, {});
}

StateTree::Transaction StateTree::begin_transaction(UndoManager* undo, std::string_view name) {
    if (flush_in_progress())
        return Transaction{};
    auto tx = active_tx();
    if (!tx) {
        StateTree* top = this;
        while (top->parent_)
            top = top->parent_;
        tx = std::make_shared<TxState>();
        tx->owner = top->shared_from_this();
        top->tx_ = tx;
    }
    if (undo && !tx->undo) {
        tx->undo = undo;
        tx->undo_name = std::string(name);
    }
    ++tx->depth;
    const size_t edit_mark = tx->edits.size();
    const size_t pending_mark = tx->pending.size();
    return Transaction(std::move(tx), edit_mark, pending_mark);
}

StateTree::ValidatedTransaction StateTree::begin_validated_transaction(PropertyValidator validator,
                                                                       UndoManager* undo,
                                                                       std::string_view name) {
    const bool reentrant = flush_in_progress();
    return ValidatedTransaction(begin_transaction(undo, name), std::move(validator), reentrant);
}

StateTree::Transaction::Transaction(Transaction&& other) noexcept
    : state_(std::move(other.state_)), edit_mark_(other.edit_mark_),
      pending_mark_(other.pending_mark_), active_(other.active_) {
    other.active_ = false;
}

StateTree::Transaction& StateTree::Transaction::operator=(Transaction&& other) noexcept {
    if (this != &other) {
        commit();
        state_ = std::move(other.state_);
        edit_mark_ = other.edit_mark_;
        pending_mark_ = other.pending_mark_;
        active_ = other.active_;
        other.active_ = false;
    }
    return *this;
}

StateTree::Transaction::~Transaction() {
    commit();
}

void StateTree::Transaction::end() {
    active_ = false;
    auto state = std::move(state_);
    if (state && --state->depth == 0)
        finish(std::move(state));
}

void StateTree::Transaction::commit() {
    if (!active_)
        return;
    end();
}

bool StateTree::Transaction::abort() {
    if (!active_ || !state_)
        return false;
    auto& s = *state_;
    const bool was_reverting = s.reverting;
    s.reverting = true;
    while (s.edits.size() > edit_mark_) {
        Edit e = std::move(s.edits.back());
        s.edits.pop_back();
        replay(e, false);
    }
    s.reverting = was_reverting;
    if (s.pending.size() > pending_mark_)
        s.pending.erase(s.pending.begin() + static_cast<std::ptrdiff_t>(pending_mark_),
                        s.pending.end());
    end();
    return true;
}

StateTree::ValidatedTransaction&
StateTree::ValidatedTransaction::operator=(ValidatedTransaction&& other) noexcept {
    if (this != &other) {
        if (is_active())
            (void)commit();
        tx_ = std::move(other.tx_);
        validator_ = std::move(other.validator_);
        first_error_ = std::move(other.first_error_);
        reentrant_ = other.reentrant_;
    }
    return *this;
}

StateTree::ValidatedTransaction::~ValidatedTransaction() {
    if (is_active())
        (void)commit();
}

TreeEditResult
StateTree::ValidatedTransaction::guard(std::initializer_list<const StateTree*> nodes) const {
    if (reentrant_)
        return edit_err(TreeEditErrorCode::kReentrantTransaction,
                        "transaction begun from a listener during a commit burst");
    if (!tx_.is_active())
        return edit_err(TreeEditErrorCode::kTransactionClosed, "transaction already ended");
    for (const StateTree* n : nodes)
        if (n->active_tx() != tx_.state_)
            return edit_err(TreeEditErrorCode::kNotInTransaction,
                            "node is not part of this transaction's tree");
    return edit_ok();
}

TreeEditResult StateTree::ValidatedTransaction::note(TreeEditResult result) {
    if (!result && !first_error_)
        first_error_ = result.error();
    return result;
}

TreeEditResult StateTree::ValidatedTransaction::set(StateTree& node, std::string_view name,
                                                    PropertyValue value) {
    if (auto g = guard({&node}); !g)
        return note(std::move(g));
    return note(node.set_checked(name, std::move(value), validator_));
}

TreeEditResult StateTree::ValidatedTransaction::remove(StateTree& node, std::string_view name) {
    if (auto g = guard({&node}); !g)
        return note(std::move(g));
    return note(node.remove_checked(name));
}

TreeEditResult StateTree::ValidatedTransaction::add_child(StateTree& parent, Ptr child) {
    if (auto g = guard({&parent}); !g)
        return note(std::move(g));
    return note(parent.add_child_checked(std::move(child)));
}

TreeEditResult StateTree::ValidatedTransaction::insert_child(StateTree& parent, int index,
                                                             Ptr child) {
    if (auto g = guard({&parent}); !g)
        return note(std::move(g));
    return note(parent.insert_child_checked(index, std::move(child)));
}

TreeEditResult StateTree::ValidatedTransaction::remove_child(StateTree& parent, int index) {
    if (auto g = guard({&parent}); !g)
        return note(std::move(g));
    return note(parent.remove_child_checked(index));
}

TreeEditResult StateTree::ValidatedTransaction::move_child(StateTree& from_parent, int from_index,
                                                           StateTree& to_parent, int to_index) {
    if (auto g = guard({&from_parent, &to_parent}); !g)
        return note(std::move(g));
    return note(StateTree::move_child_checked(from_parent, from_index, to_parent, to_index));
}

TreeEditResult StateTree::ValidatedTransaction::commit() {
    if (!tx_.is_active())
        return edit_err(reentrant_ ? TreeEditErrorCode::kReentrantTransaction
                                   : TreeEditErrorCode::kTransactionClosed,
                        "transaction already ended");
    if (first_error_) {
        tx_.abort();
        return TreeEditResult(runtime::Err(*first_error_));
    }
    tx_.commit();
    return edit_ok();
}

bool StateTree::ValidatedTransaction::abort() {
    return tx_.abort();
}

// ── Public edits ────────────────────────────────────────────────────────

void StateTree::set(std::string_view name, PropertyValue value) {
    apply_property(name, std::move(value));
}

TreeEditResult StateTree::set_checked(std::string_view name, PropertyValue value,
                                      const PropertyValidator& validator) {
    if (name.empty())
        return edit_err(TreeEditErrorCode::kInvalidName, "property name is empty");
    if (validator) {
        if (auto message = validator(*this, name, value))
            return edit_err(TreeEditErrorCode::kRejectedByValidator, std::move(*message));
    }
    apply_property(name, std::move(value));
    return edit_ok();
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
    apply_property(name, std::nullopt);
}

TreeEditResult StateTree::remove_checked(std::string_view name) {
    if (name.empty())
        return edit_err(TreeEditErrorCode::kInvalidName, "property name is empty");
    if (!has(name))
        return edit_err(TreeEditErrorCode::kMissingProperty,
                        "no property named '" + std::string(name) + "'");
    apply_property(name, std::nullopt);
    return edit_ok();
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

int StateTree::index_of(const StateTree* node) const {
    for (int i = 0; i < child_count(); ++i)
        if (children_[static_cast<size_t>(i)].get() == node)
            return i;
    return -1;
}

TreeEditResult StateTree::insert_child_checked(int index, Ptr child) {
    if (!child)
        return edit_err(TreeEditErrorCode::kNullChild, "child is null");
    // A node cannot become its own descendant: such a cycle leaks a
    // shared_ptr ring and makes deep_copy() / to_json() recurse forever.
    if (child->is_ancestor_of(this))
        return edit_err(TreeEditErrorCode::kWouldCreateCycle,
                        "child is this node or one of its ancestors");
    const int limit = child_count() - (child->parent_ == this ? 1 : 0);
    if (index < 0 || index > limit)
        return edit_err(TreeEditErrorCode::kIndexOutOfRange, "index " + std::to_string(index) +
                                                                 " outside [0, " +
                                                                 std::to_string(limit) + "]");
    const StateTree* source = child->parent_ ? child->parent_ : child.get();
    if (auto r = check_tx(source, this); !r)
        return r;
    relocate(child, this, index, false);
    return edit_ok();
}

TreeEditResult StateTree::add_child_checked(Ptr child) {
    if (!child)
        return edit_err(TreeEditErrorCode::kNullChild, "child is null");
    return insert_child_checked(child_count() - (child->parent_ == this ? 1 : 0), std::move(child));
}

void StateTree::add_child(Ptr child) {
    (void)add_child_checked(std::move(child));
}

void StateTree::insert_child(int index, Ptr child) {
    if (!child) return;
    const int limit = child_count() - (child->parent_ == this ? 1 : 0);
    (void)insert_child_checked(std::clamp(index, 0, std::max(limit, 0)), std::move(child));
}

TreeEditResult StateTree::remove_child_checked(int index) {
    if (index < 0 || index >= child_count())
        return edit_err(TreeEditErrorCode::kIndexOutOfRange,
                        "index " + std::to_string(index) + " outside [0, " +
                            std::to_string(child_count()) + ")");
    // Copy the shared_ptr so the child stays alive while listeners run; the
    // child is erased before dispatch so a re-entrant mutation from a
    // callback sees the tree in its post-removal shape.
    auto child = children_[static_cast<size_t>(index)];
    relocate(child, nullptr, -1, false);
    return edit_ok();
}

void StateTree::remove_child(int index) {
    (void)remove_child_checked(index);
}

void StateTree::remove_child(StateTree* child) {
    int i = index_of(child);
    if (i >= 0)
        remove_child(i);
}

void StateTree::remove_all_children() {
    if (children_.empty())
        return;
    auto tx = active_tx();
    auto self = shared_from_this();
    auto removed = std::move(children_);
    children_.clear();
    const int n = static_cast<int>(removed.size());
    // Record from the last child backwards so an abort (which replays in
    // reverse) re-inserts them front to back at their original indices.
    for (int i = n - 1; i >= 0; --i) {
        auto& child = removed[static_cast<size_t>(i)];
        child->parent_ = nullptr;
        if (tx && !tx->reverting) {
            if (!(child->tx_ && child->tx_->open)) {
                child->tx_ = tx;
                tx->holders.push_back(child);
            }
            Edit e;
            e.kind = Edit::Kind::kRemove;
            e.node = child;
            e.parent = self;
            e.index = i;
            tx->edits.push_back(std::move(e));
        }
    }
    for (int i = n - 1; i >= 0; --i) {
        auto& child = removed[static_cast<size_t>(i)];
        Pending removed_note;
        removed_note.kind = Pending::Kind::kChildRemoved;
        removed_note.node = self;
        removed_note.child = child;
        removed_note.index = i;
        emit(std::move(removed_note), tx.get());
        Pending parent_note;
        parent_note.kind = Pending::Kind::kParentChanged;
        parent_note.node = child;
        parent_note.other = self;
        emit(std::move(parent_note), tx.get());
    }
}

TreeEditResult StateTree::move_child_checked(StateTree& from_parent, int from_index,
                                             StateTree& to_parent, int to_index) {
    if (from_index < 0 || from_index >= from_parent.child_count())
        return edit_err(TreeEditErrorCode::kIndexOutOfRange,
                        "source index " + std::to_string(from_index) + " outside [0, " +
                            std::to_string(from_parent.child_count()) + ")");
    auto child = from_parent.children_[static_cast<size_t>(from_index)];
    if (child->is_ancestor_of(&to_parent))
        return edit_err(TreeEditErrorCode::kWouldCreateCycle,
                        "destination is the moved child or one of its descendants");
    const bool same = &from_parent == &to_parent;
    const int limit = to_parent.child_count() - (same ? 1 : 0);
    if (to_index < 0 || to_index > limit)
        return edit_err(TreeEditErrorCode::kIndexOutOfRange,
                        "destination index " + std::to_string(to_index) + " outside [0, " +
                            std::to_string(limit) + "]");
    if (auto r = check_tx(&from_parent, &to_parent); !r)
        return r;
    if (same && from_index == to_index)
        return edit_ok();
    relocate(child, &to_parent, to_index, true);
    return edit_ok();
}

bool StateTree::move_child(StateTree& from_parent, int from_index, StateTree& to_parent,
                           int to_index) {
    return static_cast<bool>(move_child_checked(from_parent, from_index, to_parent, to_index));
}

StateTree::Ptr StateTree::child(int index) const {
    if (index < 0 || index >= child_count()) return nullptr;
    return children_[static_cast<size_t>(index)];
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

void StateTree::for_each_descendant(const std::function<void(StateTree&)>& fn) {
    if (!fn)
        return;
    auto level = children_;
    for (auto& c : level) {
        fn(*c);
        c->for_each_descendant(fn);
    }
}

StateTree::Ptr
StateTree::find_descendant(const std::function<bool(const StateTree&)>& predicate) const {
    if (!predicate)
        return nullptr;
    for (auto& c : children_) {
        if (predicate(*c))
            return c;
        if (auto found = c->find_descendant(predicate))
            return found;
    }
    return nullptr;
}

bool StateTree::is_equivalent_to(const StateTree& other) const {
    if (this == &other)
        return true;
    if (type_name_ != other.type_name_)
        return false;
    if (properties_ != other.properties_)
        return false;
    if (children_.size() != other.children_.size())
        return false;
    for (size_t i = 0; i < children_.size(); ++i)
        if (!children_[i]->is_equivalent_to(*other.children_[i]))
            return false;
    return true;
}

int StateTree::add_listener(TreeListener listener, ListenerDelivery delivery) {
    return add_slot(listeners_, std::move(listener), delivery);
}

void StateTree::remove_listener(int id) {
    remove_slot(listeners_, id);
}

int StateTree::add_child_added_listener(ChildListener listener, ListenerDelivery delivery) {
    return add_slot(child_added_listeners_, std::move(listener), delivery);
}

int StateTree::add_child_removed_listener(ChildListener listener, ListenerDelivery delivery) {
    return add_slot(child_removed_listeners_, std::move(listener), delivery);
}

void StateTree::remove_child_added_listener(int id) {
    remove_slot(child_added_listeners_, id);
}

void StateTree::remove_child_removed_listener(int id) {
    remove_slot(child_removed_listeners_, id);
}

int StateTree::add_child_moved_listener(ChildMovedListener listener, ListenerDelivery delivery) {
    return add_slot(child_moved_listeners_, std::move(listener), delivery);
}

void StateTree::remove_child_moved_listener(int id) {
    remove_slot(child_moved_listeners_, id);
}

int StateTree::add_parent_changed_listener(ParentChangedListener listener,
                                           ListenerDelivery delivery) {
    return add_slot(parent_changed_listeners_, std::move(listener), delivery);
}

void StateTree::remove_parent_changed_listener(int id) {
    remove_slot(parent_changed_listeners_, id);
}

int StateTree::add_ancestor_changed_listener(AncestorChangedListener listener,
                                             ListenerDelivery delivery) {
    return add_slot(ancestor_changed_listeners_, std::move(listener), delivery);
}

void StateTree::remove_ancestor_changed_listener(int id) {
    remove_slot(ancestor_changed_listeners_, id);
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
            src->remove_child_moved_listener(w.child_moved_listener_id);
        }
    }
    wiring_.clear();
    attached_ = false;
}

void StateTree::SyncedClone::detach_recursive(StateTree& src) {
    for (int i = 0; i < src.child_count(); ++i)
        if (auto c = src.child(i))
            detach_recursive(*c);
    for (auto it = wiring_.begin(); it != wiring_.end();) {
        auto locked = it->source.lock();
        if (locked.get() == &src) {
            src.remove_listener(it->prop_listener_id);
            src.remove_child_added_listener(it->child_added_listener_id);
            src.remove_child_removed_listener(it->child_removed_listener_id);
            src.remove_child_moved_listener(it->child_moved_listener_id);
            it = wiring_.erase(it);
        } else {
            ++it;
        }
    }
}

void StateTree::SyncedClone::attach_recursive(StateTree& src, StateTree& dst) {
    // Property changes: set / remove on src → set / remove on dst.
    int prop_id = src.add_listener(
        [dst_ptr = &dst](StateTree& /*node*/, std::string_view prop, const PropertyValue& /*old*/,
                         const PropertyValue& new_v) {
            if (std::holds_alternative<std::monostate>(new_v)) {
                dst_ptr->remove(prop);
            } else {
                dst_ptr->set(prop, new_v);
            }
        },
        ListenerDelivery::kImmediate);

    // Child-added on src → deep-copy onto dst at the same index AND wire
    // the new subtree recursively so deeper mutations keep mirroring.
    int added_id = src.add_child_added_listener(
        [this, dst_ptr = &dst](StateTree& /*parent*/, StateTree& added_child, int idx) {
            auto cloned_child = added_child.deep_copy();
            dst_ptr->insert_child(idx, cloned_child);
            this->attach_recursive(added_child, *cloned_child);
        },
        ListenerDelivery::kImmediate);

    // Child-removed on src → unhook the removed subtree's wiring, then
    // remove the same index on dst. A re-add (or the second half of a move)
    // wires the subtree afresh through the child-added handler.
    int removed_id = src.add_child_removed_listener(
        [this, dst_ptr = &dst](StateTree& /*parent*/, StateTree& removed_child, int idx) {
            // The removed source subtree may be re-added later (an aborted
            // transaction does exactly that); its old wiring points at clone
            // nodes that are about to be dropped, so unhook it now.
            this->detach_recursive(removed_child);
            dst_ptr->remove_child(idx);
        },
        ListenerDelivery::kImmediate);

    // Reorder within src → same reorder on dst.
    int moved_id = src.add_child_moved_listener(
        [dst_ptr = &dst](StateTree& /*parent*/, StateTree& /*moved_child*/, int from, int to) {
            StateTree::move_child(*dst_ptr, from, *dst_ptr, to);
        },
        ListenerDelivery::kImmediate);

    // weak_ptr so dropped subtrees don't dangle here — see WiringEntry
    // comment in the header.
    wiring_.push_back({src.weak_from_this(), prop_id, added_id, removed_id, moved_id});

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
