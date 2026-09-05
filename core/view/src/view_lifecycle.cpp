// The root-owned mutation/lifetime gate. Contract: view_lifecycle.hpp.

#include <pulp/view/view_lifecycle.hpp>

#include <pulp/view/view.hpp>

#include <utility>

namespace pulp::view {

namespace {

View* tree_root_of(View& view) noexcept {
    View* root = &view;
    while (root->parent()) root = root->parent();
    return root;
}

} // namespace

DispatchLease::DispatchLease(View& view, Drain drain) noexcept
    : view_(&view), root_(tree_root_of(view)), drain_(drain) {
    // Opening a pass is the safe moment to free what a previous `deferred`
    // pass left behind: that callback has fully returned, so its closure is no
    // longer executing.
    if (root_->lease_depth_ == 0) root_->drain_retired();
    // Both counters are raised here and lowered from the stored pointers, so a
    // hook that detaches `view` (moving it to a different root) still lowers
    // exactly the depths this lease raised.
    ++root_->lease_depth_;
    ++view_->dispatch_depth_;
}

DispatchLease::~DispatchLease() {
    if (view_->dispatch_depth_ > 0) --view_->dispatch_depth_;
    if (root_->lease_depth_ == 0) return;
    if (--root_->lease_depth_ == 0 && drain_ == Drain::at_exit)
        root_->drain_retired();
}

void View::retire(std::unique_ptr<View> owned) noexcept {
    if (!owned) return;
    View* root = tree_root_of(*this);
    if (root->lease_depth_ == 0) {
        // Nothing is executing on this tree, so the ordinary destruction path
        // is already safe. Freeing here keeps `retire` usable unconditionally.
        owned.reset();
        return;
    }
    // Park it. Threading the previous head through the retired view's own link
    // means no container grows, so this stays valid on a noexcept path.
    owned->retired_next_ = std::move(root->retired_head_);
    root->retired_head_ = std::move(owned);
}

void View::drain_retired() noexcept {
    while (retired_head_) {
        std::unique_ptr<View> node = std::move(retired_head_);
        // Unlink before destroying so the chain is popped iteratively rather
        // than recursing through each retired view's member destructor.
        retired_head_ = std::move(node->retired_next_);
    }
}

} // namespace pulp::view
