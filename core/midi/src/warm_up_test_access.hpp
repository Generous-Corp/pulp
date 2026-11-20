#pragma once

#include <functional>

namespace pulp::midi::detail {

/// Starts a warm-up that runs `work` instead of the platform probe. Same
/// once-only rule as warm_up(); returns false if a warm-up was already started.
bool start_warm_up(std::function<void()> work);

/// Forgets a finished warm-up so a test can start another. Waits for a running
/// one first.
void reset_warm_up_for_testing() noexcept;

} // namespace pulp::midi::detail
