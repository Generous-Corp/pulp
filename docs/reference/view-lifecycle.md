# View lifecycle and ownership contract

A `View` calls code it does not control — `on_attached()`, `on_detached()`,
`on_frame_clock_changed()`, focus and overlay hooks, and every `std::function`
slot a widget exposes — and then keeps executing on `this`. In an imported UI
that code is a JavaScript handler, so "user code" means "anything the design
author wrote". This page states the rules that make that safe.

The header is
[`core/view/include/pulp/view/view_lifecycle.hpp`](../../core/view/include/pulp/view/view_lifecycle.hpp);
the tests are `test/test_view_lifecycle_contract.cpp`.

## The three rules

**1. Mutation from a callback is allowed and commits immediately.**
A hook may add, remove, move, or replace views. Nothing is queued and nothing is
rejected. Observe the result through the mutating call's return value or a
`ViewCapture`; never through a pointer you captured before the callback.

**2. Destruction from a callback is deferred.**
Destroying a view while one of its own callbacks is running frees a frame that
is still executing. So a caller that wants a removed view destroyed from inside
a callback hands it to `View::retire()`:

```cpp
void MyWidget::on_frame_clock_changed() {
    if (should_disappear()) {
        View* parent = this->parent();
        root().retire(parent->remove_child(this));   // safe; `this` stays alive
        // execution continues here legally
    }
}
```

Outside a callback `retire()` destroys immediately, so it is always safe to
call. Retiring allocates nothing, which is why a `noexcept` path can use it.

**3. Destroying an attached or actively-dispatched view is a contract
violation.** Call `remove_child()` first, or `retire()`. Debug builds assert;
release builds are unchanged. The one legitimate exception is a child destroyed
as part of its parent's own teardown, which the contract recognizes.

## Identity is address **and** instance id

A raw `View*` is not an identity. A callback can destroy a view and the
allocator can place a new one at the same address, so an address-only re-find
can operate on the wrong view (an ABA). Every re-find after a callback compares
the address **and** `import_binding_instance_id()`:

```cpp
const auto child_id = child->import_binding_instance_id();
const auto is_child = [child, child_id](const auto& candidate) {
    return candidate.get() == child &&
           candidate->import_binding_instance_id() == child_id;
};
```

`ViewCapture` already encapsulates this for cross-callback references and is the
preferred tool; prefer it to a raw pointer whenever a hook runs in between.

`remove_child()` returns ownership when the argument is still this parent's
child under the same instance id, and `nullptr` otherwise. **A null result means
"not ours anymore" — never "moved for you".**

## Attach and detach are transactions

`add_child_transactional()`:

1. clears `detaching_` across the incoming subtree and links `parent_`;
2. inserts into `children_` — the child is fully linked before any hook runs;
3. propagates the host surfaces (virtual, therefore user code);
4. invokes `on_attached()` **on the moved root only**, under a lease;
5. notifies the frame clock across the subtree.

A throw anywhere from step 3 onward unwinds to: not inserted, `parent_` null,
`detaching_` true, `on_detached()` delivered once, and the caller's
`unique_ptr` restored. Host propagation is inside the transaction precisely
because it is virtual: outside it, a throwing override left a child whose
`parent_` already pointed at the new parent with no unwind path.

`remove_child()` retires gesture, drag, popup, and focus state while the subtree
is still attached, sets `detaching_` across the subtree, invokes `on_detached()`
on the moved root under a lease, then severs hosts, extracts ownership,
publishes the structure generation, and notifies the detached subtree's clock.

**`on_attached()` / `on_detached()` fire on the moved root only, not on
descendants.** This is the long-standing public contract and does not change.
The descendant-facing lifecycle funnel is `on_frame_clock_changed()`. A future
requirement for subtree structural hooks should add an opt-in
`on_subtree_attached` / `on_subtree_detached` pair rather than redefining these.

## Frame-clock propagation is exactly-once

`notify_frame_clock_changed()` must notify every node attached under it when the
call began exactly once, survive arbitrary mutation from the hooks it invokes,
and allocate nothing (it is `noexcept`). It uses two stamps rather than scratch
storage:

- a **traversal epoch**, bumped per pass, so one pass visits each node at most
  once even as `children_` is mutated underneath it;
- a **delivery sequence**, bumped once per call, so the call as a whole notifies
  each node at most once — a later pass may walk *through* an already-notified
  node to reach a descendant attached mid-pass.

Children are found by rescanning `children_` from index 0 after every hook, not
by a saved index. An index does not survive a callback: an earlier sibling may
have been removed, so the same index now names a different node. The previous
index walker demonstrably skipped nodes — remove yourself and an earlier sibling
from a hook and the next sibling was never notified.

A node attached *during* a pass is stamped with that pass's traversal epoch, so
the running pass skips it and a hook that attaches on every notification cannot
extend the walk without end. Its delivery stamp is left alone, so a follow-up
pass notifies it exactly once. Follow-up passes are bounded: a hook that
generates work indefinitely loses the remaining notifications rather than
hanging the UI thread.

## Exceptions: first error wins

Cleanup and rollback run under `catch (...)` and complete even if they
themselves fail. **A failing rollback must never replace the original
exception**, and must never let a view die on unwind while a non-owning registry
still names it. When neither parent will accept a view back, drop the registry
records first, then `retire()` it, then rethrow the original error.

## Bridge integration

The retirement queue belongs to the tree root, not to `WidgetBridge`, so a view
removed by a JS handler and one removed by a native hook obey one contract with
one owner. `BridgeCallbackScope` raises the root's gate for the duration of a
native callback. That lease is **deferred-drain**: the scope is constructed as
the first statement of the callback's own `std::function` body, so its
destructor still runs inside `std::function::operator()` — draining there could
destroy the closure that is executing. Retired views are instead freed when the
next outermost lease opens.

Bridge registries (`widgets_`, `owned_widgets_`, `scroll_wrappers_`) are
non-owning caches. **Publish an identity only after the native transaction
commits**, so a rejected attach cannot leave a registry naming a view that is
about to be destroyed.

## Why this matters for imported UIs

Every design-import lane — Figma, Claude Design HTML, JSX/React, Stitch, v0,
Pencil — reaches the native tree through the same `__domAppend` / `__domRemove`
path, so all of them inherit this contract with no per-lane code. Re-running an
`edit source → reimport → materialize` loop is exactly the workload that stresses
it: retained identity is preserved across reparenting, a container can be
upgraded to a `ScrollView` in place, and the callbacks that fire during those
moves are author-written and free to mutate the tree.
