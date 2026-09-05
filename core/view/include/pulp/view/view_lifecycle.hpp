#pragma once

/// \file
/// The View mutation/lifetime contract.
///
/// A View invokes overridable code — `on_attached()`, `on_detached()`,
/// `on_frame_clock_changed()`, focus/overlay hooks, bridge callbacks — and then
/// keeps executing on `this`. Without a gate, such a hook can remove and
/// destroy the very View whose member is running, so the member returns into
/// freed memory.
///
/// The contract closing that hole has three parts:
///
/// 1. **Every call into overridable code is made under a `DispatchLease`**,
///    taken at the call site inside the member — never inside the closure, so
///    the lease outlives the `std::function::operator()` expression itself.
/// 2. **Destruction is deferred, mutation is not.** A hook may freely add,
///    remove, move, or replace views; those mutations commit immediately.
///    Only *destroying* a view is deferred: a caller that wants a removed view
///    destroyed from inside a callback hands it to `View::retire()`, and the
///    root frees it once the outermost lease exits.
/// 3. **Destroying an attached or actively-dispatched View is an invariant
///    violation**, asserted in debug builds. `remove_child()` first, or
///    `retire()`.
///
/// The gate is owned by the *tree root* — one ownership authority per tree —
/// and the retirement chain is intrusive, so retiring inside a `noexcept`
/// walker allocates nothing.
///
/// See docs/reference/view-lifecycle.md for the full contract.

namespace pulp::view {

class View;

/// RAII gate held across any call into overridable View code.
///
/// Construction walks to the tree root and raises that root's lease depth plus
/// the leased view's own dispatch depth; destruction lowers both and, when the
/// root's depth returns to zero, destroys everything retired during the pass.
///
/// Take one at the call site inside the member that invokes the hook:
///
/// ```cpp
/// {
///     DispatchLease lease(*child);
///     child->on_attached();          // may remove/destroy `child`
/// }                                  // retirement drains here (outermost)
/// // `child` is still alive above; re-validate identity before using it.
/// ```
///
/// A lease keeps the view's *storage* alive; it does not keep it *attached*.
/// A hook may still detach or replace the view, so re-validate identity
/// (address **and** `import_binding_instance_id()`, or a `ViewCapture`) after
/// every hook returns.
class DispatchLease {
public:
    /// When the outermost lease on a tree is released, is it safe to destroy
    /// what was retired during the pass?
    enum class Drain {
        /// Yes. The hook has already returned, so nothing retired during it can
        /// still be executing. This is the case for every View-internal call
        /// site, where the lease brackets the hook rather than living inside it.
        at_exit,
        /// No. The lease itself lives *inside* the callback it guards — a
        /// bridge callback takes its scope as the first statement of the
        /// `std::function` body, so the lease destructor still runs inside
        /// `std::function::operator()`. Destroying a retired view there can
        /// free the very closure that is executing. Retirement instead carries
        /// over and drains when the next outermost lease is taken.
        deferred,
    };

    explicit DispatchLease(View& view, Drain drain = Drain::at_exit) noexcept;
    ~DispatchLease();

    DispatchLease(const DispatchLease&) = delete;
    DispatchLease& operator=(const DispatchLease&) = delete;
    DispatchLease(DispatchLease&&) = delete;
    DispatchLease& operator=(DispatchLease&&) = delete;

    /// The root whose gate this lease raised. Stored at construction so a lease
    /// released after its view was detached still lowers the depth it raised.
    View* root() const noexcept { return root_; }

private:
    View* view_ = nullptr;
    View* root_ = nullptr;
    Drain drain_ = Drain::at_exit;
};

} // namespace pulp::view
