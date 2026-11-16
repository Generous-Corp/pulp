#pragma once

#include <mutex>

namespace pulp::rmlui_native::detail {

// The lock is process-wide: its current owner need not have input of its own
// or another frame scheduled for this surface. Keep our own retry on contention.
template <class Retry, class Apply>
bool try_drain_input(std::mutex& mutex, Retry retry, Apply apply) {
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        retry();
        return false;
    }
    return apply();
}

} // namespace pulp::rmlui_native::detail
