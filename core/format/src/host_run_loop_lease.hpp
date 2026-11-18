#pragma once

// A host's run loop, lent to the platform layer for as long as an editor can
// run (see pulp/platform/main_thread.hpp, HostRunLoop).
//
// Where the main thread is a plug-in host's (Linux), an adapter builds a
// platform::HostRunLoop over the host's own API (VST3 IRunLoop, CLAP
// timer-support and posix-fd-support) and holds one of these while its editor
// exists. Construction registers the loop and opens a process-wide scope, so
// post_to_main_thread() from any thread runs on the host's UI thread, and
// MainThreadScope / MainThreadTimer made by the editor use the loop. Destroying
// it stops all of that and unregisters the loop; the lease must be destroyed
// after the editor it served.
//
// Main (UI) thread only. A no-op on Apple platforms, which have a main run
// loop of their own.

#include <pulp/platform/main_thread.hpp>

#include <memory>

namespace pulp::format::detail {

class HostRunLoopLease {
  public:
    /// Register `loop`, which must outlive the lease. `process_scope` opens the
    /// scope that post_to_main_thread() uses; a host that cannot watch
    /// descriptors should leave it off, because that scope would then wake the
    /// UI thread by polling a timer for as long as the lease lives (an editor's
    /// own scopes still work, and pay for polling only while they exist).
    explicit HostRunLoopLease(platform::HostRunLoop& loop, bool process_scope = true);
    ~HostRunLoopLease();
    HostRunLoopLease(const HostRunLoopLease&) = delete;
    HostRunLoopLease& operator=(const HostRunLoopLease&) = delete;

    /// Whether the host's loop was accepted.
    bool active() const {
        return token_ != 0;
    }

  private:
    platform::HostRunLoopToken token_ = 0;
    std::unique_ptr<platform::MainThreadScope> scope_;
};

/// Post `fn` to the host's UI thread through the innermost live lease, from any
/// thread. False when no lease exists (no editor is open), leaving `fn` as it
/// was.
bool post_through_host_run_loop(std::function<void()>& fn);

} // namespace pulp::format::detail
