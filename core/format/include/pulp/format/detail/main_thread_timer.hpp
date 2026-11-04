#pragma once

// A repeating main-thread timer that stops for good.
//
// An editor's idle tick runs code from the plug-in binary on the host's main
// thread. A chain of one-shot posts (MainThreadDispatcher::call_async_after)
// cannot be withdrawn: after the editor closes, one already-queued post still
// runs plug-in code up to an interval later, and a host that unloads the
// bundle right after closing would run unmapped code. This timer is a
// cancellable source instead. On Apple platforms it is a dispatch timer source
// on the main queue: stop() cancels it, after which libdispatch invokes the
// handler no more, and it never interrupts a tick in progress because both run
// on the main thread. Elsewhere it falls back to the post chain, which stops
// ticking at stop() but may still run one guarded post.
//
// start() and stop() run on the main thread. stop() from another thread waits
// for the main thread to cancel the source.

#include <functional>
#include <memory>

namespace pulp::format::detail {

class MainThreadTimer {
  public:
    MainThreadTimer();
    ~MainThreadTimer();
    MainThreadTimer(const MainThreadTimer&) = delete;
    MainThreadTimer& operator=(const MainThreadTimer&) = delete;

    /// Call `tick` on the main thread every `interval_ms` until stop(). Stops a
    /// running timer first. False when this platform cannot schedule it.
    bool start(int interval_ms, std::function<void()> tick);
    /// Stop ticking. After it returns, `tick` is not called again.
    void stop();
    bool running() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::format::detail
