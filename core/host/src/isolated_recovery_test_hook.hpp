// Test seam for SignalGraph::publish_isolated_recovery().
//
// The replacement worker is prepared without the graph lock, then revalidated
// under it before publication. A worker can exit inside that window, which no
// fixture command can hit deterministically, so a test installs a hook that
// runs exactly there. Production never installs one; the hook is internal to
// the host sources and is not part of the public SDK.
#pragma once

namespace pulp::host {
class PluginSlot;
}

namespace pulp::host::detail {

using IsolatedRecoveryPreparedHook = void (*)(PluginSlot& replacement, void* context);

/// Installs (or, with nullptr, clears) the hook. Runs on the recovering
/// control thread after a successful unlocked prepare and before the locked
/// revalidation. Not thread-safe against a concurrent recovery that is already
/// between those two points.
void set_isolated_recovery_prepared_hook_for_test(IsolatedRecoveryPreparedHook hook,
                                                  void* context) noexcept;

} // namespace pulp::host::detail
