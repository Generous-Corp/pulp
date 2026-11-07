---
breaking = false
summary = "StateTree gains transactions, undo steps, move_child and checked edits"
---

## What changed
- `StateTree::begin_transaction()` returns a move-only `Transaction`. Edits
  apply at once; coalesced listeners get one burst on commit, and `abort()`
  reverts. `begin_transaction(UndoManager*, name)` records the transaction as
  one undo step. `begin_validated_transaction()` returns edits as
  `TreeEditResult` values.
- New: `move_child`, `remove_all_children`, `index_of`, `for_each_descendant`,
  `find_descendant`, `is_equivalent_to`, the `*_checked` edit methods, and
  `child_moved` / `parent_changed` / `ancestor_changed` listeners.
- Every `add_*_listener` takes an optional `ListenerDelivery`. The default,
  `kCoalesced`, behaves exactly as before outside a transaction.
- `StateTreeSynchroniser` emits a new `SyncDeltaType::ChildMove` delta for
  reorders made with `move_child`.

## Behavior changes
- When `add_child()` or `insert_child()` moves a node that already has a
  parent, the old parent's `child_removed` listeners now run after the node is
  attached to its new parent. Before, they ran in between. Listeners that read
  `child.parent()` in that callback now see the new parent.
- `clone_synced()` now unhooks a removed subtree's wiring, so re-adding the
  same subtree no longer leaves a duplicate set of listeners on it.

## Migration steps
- No changes are needed for existing callers.
- A listener that must see every individual edit inside a transaction should
  pass `ListenerDelivery::kImmediate` when it registers.
- A receiver of `StateTreeSynchroniser` deltas built against an older Pulp
  stops decoding at a `ChildMove` delta. Update both ends together.
