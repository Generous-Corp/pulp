#include <pulp/view/slider_core.hpp>

namespace pulp::view {

namespace {

// UI-thread only, like the rest of the view input/paint path. A plain
// function-local static keeps the queue out of the header and out of static-init
// order (it is constructed on first use, destroyed after main).
std::vector<std::function<void()>>& pending_queue() {
    static std::vector<std::function<void()>> q;
    return q;
}

} // namespace

void queue_async_notification(std::function<void()> fn) {
    if (fn) pending_queue().push_back(std::move(fn));
}

void flush_async_notifications() {
    auto& q = pending_queue();
    if (q.empty()) return;
    // Swap the queue out before running it. A callback that queues another
    // notification appends to the (now empty) live queue, so it runs on the NEXT
    // flush — this loop cannot be extended from inside itself, and the iterator
    // cannot be invalidated by a push during the walk.
    std::vector<std::function<void()>> batch;
    batch.swap(q);
    for (auto& fn : batch) fn();
}

} // namespace pulp::view
