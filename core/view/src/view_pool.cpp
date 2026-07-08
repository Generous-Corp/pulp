#include <pulp/view/view_pool.hpp>

#include <utility>

namespace pulp::view {

std::unique_ptr<View> ViewPool::take(std::type_index key) {
    auto it = free_lists_.find(key);
    if (it == free_lists_.end() || it->second.empty()) return nullptr;
    auto view = std::move(it->second.back());
    it->second.pop_back();
    return view;
}

std::unique_ptr<View> ViewPool::acquire(std::type_index key) {
    if (auto recycled = take(key)) {
        recycled->prepare_for_reuse();
        return recycled;
    }
    return nullptr;
}

void ViewPool::release(std::unique_ptr<View> view) {
    if (!view) return;
    // Opt-in only: a subclass that does not override supports_reuse() is
    // destroyed here as `view` leaves scope, exactly as it would be without a
    // pool. Keeps adoption behavior-neutral.
    if (!view->supports_reuse()) return;
    // Key on the DYNAMIC type so a Derived is only ever handed back to an
    // acquire<Derived>() / acquire(typeid(Derived)) — never cross-class.
    auto& list = free_lists_[std::type_index(typeid(*view))];
    if (list.size() >= per_class_cap_) return;  // over cap → destroyed
    list.push_back(std::move(view));
}

}  // namespace pulp::view
