# State Module: StateTree Transactions and Undo

`StateTree` is the reactive hierarchical store in `pulp::state`. This guide
covers grouped edits: transactions, undo/redo steps, moves, checked edits for
scripted or remote callers, and the listener contract they rely on.

**Status**: stable
**Dependencies**: runtime
**Headers**: `pulp/state/state_tree.hpp`, `pulp/state/undo_manager.hpp`

Every API here is message-thread only. StateTree is not thread-safe; audio
threads exchange parameters through `StateStore` instead.

## Transactions

`begin_transaction()` returns a move-only RAII object. Edits made while it is
open apply to the tree at once, but listener notifications are held and
delivered as one burst when the transaction ends.

```cpp
#include <pulp/state/state_tree.hpp>
using namespace pulp::state;

{
    auto tx = root->begin_transaction();
    root->set("gain", 0.8);
    root->add_child(StateTree::create("voice"));
    root->child(0)->set("detune", 3.0);
}   // commit: listeners receive one burst here
```

- **Scope.** A transaction covers the whole tree. It is opened on the topmost
  ancestor, so it does not matter which node you begin it from.
- **Nesting.** A transaction begun while one is open joins it. The burst is
  delivered when the last open transaction ends.
- **Abort.** `abort()` reverts the edits made since that transaction began,
  in reverse order, and drops their notifications. An inner abort keeps the
  outer transaction's earlier edits.
- **Detached nodes.** A subtree removed during the transaction stays covered.
  Edits made to it are reverted by abort as well.

The burst keeps edit order. Repeated changes to one property of one node
merge into a single notification carrying the first old value and the last
new value. A change whose net effect is nothing is dropped. Structural
notifications arrive one per edit, with the indices the edit used, while the
tree is already in its final state.

### Listener delivery

Listeners default to `ListenerDelivery::kCoalesced`, which gives the burst
behavior above. Observers that must replay every step register with
`ListenerDelivery::kImmediate`. They fire after each edit inside a
transaction, and they also see the inverse edits an abort applies.
`StateTreeSynchroniser` and `clone_synced()` use immediate delivery, so mirrors
and IPC deltas stay in step through transactions and aborts.

```cpp
root->add_listener(
    [](StateTree& node, std::string_view prop, const PropertyValue&, const PropertyValue&) {
        // runs for every edit, including reverts
    },
    ListenerDelivery::kImmediate);
```

### Reentrancy

A listener running inside a commit burst may edit the tree directly. Those
edits notify inline. It must not begin a transaction on the same tree: that
returns an inert transaction whose `is_active()` is false. A validated
transaction begun there refuses every edit with `kReentrantTransaction`.

## Undo and redo

Pass an `UndoManager` and a name, and the whole transaction becomes one undo
step when the outermost transaction commits.

```cpp
UndoManager undo;
{
    auto tx = root->begin_transaction(&undo, "Rearrange voices");
    StateTree::move_child(*root, 0, *root, 2);
    root->remove_child(1);
    root->set("mode", std::string("mono"));
}
undo.undo();   // restores all three edits, one notification burst
undo.redo();
```

Set, remove, add, insert, remove-child, remove-all-children and move are
recorded. Children are restored by identity, so undo still works after
unrelated edits have shifted indices. An aborted or empty transaction pushes
no step. Several tree transactions inside one `UndoManager::begin_transaction`
group become a single step.

## Moves and the listener contract

`StateTree::move_child(from_parent, from_index, to_parent, to_index)` moves a
child. `to_index` is the child's final position.

- **Reorder within one parent.** Listeners get `child_moved(parent, child,
  old_index, new_index)`.
- **Move to another parent.** Listeners get `child_removed` on the old parent,
  `child_added` on the new one, `parent_changed` on the child, and
  `ancestor_changed` on the child and every descendant.

Listeners run after the edit, so they see the final state. A `child_removed`
callback for a move already sees the new parent. `add_child()` of a node that
has a parent follows the same rule. `remove_all_children()` detaches every
child first, then notifies from the last child backwards.

## Searching and comparing

```cpp
root->for_each_descendant([](StateTree& n) { /* depth-first, parents first */ });
auto lfo = root->find_descendant([](const StateTree& n) { return n.type_name() == "lfo"; });
bool same = root->is_equivalent_to(*snapshot);  // type, properties, ordered children
```

## Checked edits for scripts and remote tools

The `*_checked` methods return `TreeEditResult`, which is
`pulp::runtime::Result<std::monostate, TreeEditError>`. A refused edit changes
nothing and notifies no one. The error carries a code and a message.

```cpp
PropertyValidator range = [](const StateTree&, std::string_view name, const PropertyValue& v)
    -> std::optional<std::string> {
    if (name == "gain" && !std::holds_alternative<double>(v)) return "gain must be a number";
    return std::nullopt;
};

auto tx = root->begin_validated_transaction(range, &undo, "Script edit");
if (auto r = tx.set(*root, "gain", std::string("loud")); !r)
    report(r.error().message);
auto result = tx.commit();   // aborts everything and returns the first error if any edit failed
```

`ValidatedTransaction` also refuses edits to nodes outside its tree with
`kNotInTransaction`. Edits spanning two trees that each have a different open
transaction are refused with `kTransactionConflict`.

Not covered yet: JSON-Schema validation of property values, and exposing
transactions to the JavaScript bridge. Neither the view nor the runtime script
bridge exposes `StateTree` today.
