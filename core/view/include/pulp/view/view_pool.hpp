#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include <pulp/view/view.hpp>

namespace pulp::view {

/// Per-class recycling pool for `View` objects.
///
/// A ViewPool keeps a bounded free-list per concrete `View` subclass, keyed by
/// `std::type_index`. Recycling avoids the allocate/construct/destroy churn that
/// dominates large virtualized lists/grids and scripted-UI hot reloads: instead
/// of destroying a row/cell View when it scrolls out of view and building a
/// fresh one when it scrolls back in, the pool hands the same object back after
/// `View::prepare_for_reuse()` scrubs its per-instance state.
///
/// Ownership model — INSTANCE-OWNED, never a process-global singleton. A
/// process-global pool would outlive the ScriptEngines / view trees that
/// populate it and invite stale-callback use-after-free: a recycled view could
/// hold a `std::function` capturing state from a torn-down engine. Each owner
/// (VirtualList, VirtualGrid, a future hot-reload recycler) holds its own
/// ViewPool whose lifetime is bounded by the owner's, so the free-list drains
/// when the owner dies.
///
/// Opt-in only. `release()` stores a view only when `view->supports_reuse()`
/// returns true; every other view is destroyed immediately, exactly as it would
/// be without a pool. This keeps adoption behavior-neutral: a subclass that does
/// not override `supports_reuse()` is never pooled.
///
/// Not thread-safe. All access must happen on the UI thread that owns the view
/// tree, the same constraint the View hierarchy already imposes.
class ViewPool {
public:
    /// Default per-class free-list cap. Sized to comfortably cover a viewport's
    /// worth of recycled rows/cells plus overscan without unbounded retention.
    static constexpr std::size_t kDefaultPerClassCap = 32;

    ViewPool() = default;
    explicit ViewPool(std::size_t per_class_cap) : per_class_cap_(per_class_cap) {}

    ViewPool(const ViewPool&) = delete;
    ViewPool& operator=(const ViewPool&) = delete;

    /// Acquire a `T`. Pops a recycled instance from the `typeid(T)` free-list,
    /// calls `prepare_for_reuse()` on it, and returns it. When the free-list is
    /// empty, calls `make()` (which must return `std::unique_ptr<T>`) and returns
    /// its result unmodified.
    ///
    /// The pool only ever stores views under their DYNAMIC type (see
    /// `release()`), so a hit here is guaranteed to be an object whose most-
    /// derived type is exactly `T` — the `static_cast` is safe and there is no
    /// cross-class handout.
    template <class T, class Factory>
    std::unique_ptr<T> acquire(Factory&& make) {
        static_assert(std::is_base_of_v<View, T>,
                      "ViewPool::acquire<T>: T must derive from pulp::view::View");
        if (auto recycled = take(std::type_index(typeid(T)))) {
            recycled->prepare_for_reuse();
            return std::unique_ptr<T>(static_cast<T*>(recycled.release()));
        }
        return std::forward<Factory>(make)();
    }

    /// Type-erased acquire for callers that do not know the concrete row/cell
    /// type at compile time (VirtualList / VirtualGrid drive a user factory that
    /// returns a base `View`). Returns a recycled, reuse-prepared view whose
    /// dynamic type is exactly `key`, or `nullptr` when the free-list is empty.
    std::unique_ptr<View> acquire(std::type_index key);

    /// Return a view to the pool. Keyed by the view's DYNAMIC type. The view is
    /// stored only when it opts in via `supports_reuse()` AND the class free-list
    /// is under the per-class cap; otherwise it is destroyed here. `prepare_for_
    /// reuse()` is deliberately NOT called on this path — it runs on the next
    /// `acquire()` so a pooled view carries no live callbacks while parked, and
    /// the reset cost is paid only for views that are actually reused.
    void release(std::unique_ptr<View> view);

    /// Set the maximum number of pooled instances per class. Does not evict
    /// entries already over the new cap; they drain naturally on `acquire()`.
    void set_per_class_cap(std::size_t cap) { per_class_cap_ = cap; }
    std::size_t per_class_cap() const { return per_class_cap_; }

    /// Drop every pooled instance across all classes.
    void clear() { free_lists_.clear(); }

    /// Number of pooled instances of a given class (introspection / tests).
    std::size_t size(std::type_index key) const {
        auto it = free_lists_.find(key);
        return it == free_lists_.end() ? 0 : it->second.size();
    }

    /// Total pooled instances across all classes (introspection / tests).
    std::size_t total_size() const {
        std::size_t n = 0;
        for (const auto& [key, list] : free_lists_) n += list.size();
        return n;
    }

private:
    /// Pop (without reset) the most-recently-released instance of `key`, or
    /// `nullptr` when the free-list is empty. Reset happens in the callers.
    std::unique_ptr<View> take(std::type_index key);

    std::size_t per_class_cap_ = kDefaultPerClassCap;
    std::unordered_map<std::type_index, std::vector<std::unique_ptr<View>>> free_lists_;
};

}  // namespace pulp::view
