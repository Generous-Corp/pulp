#include <pulp/format/detail/main_thread_timer.hpp>

#include <pulp/events/main_thread_dispatcher.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

#if defined(__APPLE__)
#include <pulp/platform/main_thread.hpp>

#include <dispatch/dispatch.h>
#include <pthread.h>
#endif

namespace pulp::format::detail {

#if defined(__APPLE__)

// The timer is a platform::MainThreadTimer in its own MainThreadScope: a
// CFRunLoopTimer in the common modes, so an editor's idle tick keeps running
// during menu tracking, live resize and modal panels, including when the
// nested loop was entered from a GCD main-queue block (which a main-queue
// dispatch timer cannot fire inside). Destroying the scope invalidates the
// timer, so nothing is left on the run loop after stop().
struct MainThreadTimer::Impl {
    std::unique_ptr<platform::MainThreadScope> scope;
    std::unique_ptr<platform::MainThreadTimer> timer;

    void stop_on_main_thread() {
        timer.reset();
        scope.reset();
    }

    static void stop_thunk(void* context) {
        static_cast<Impl*>(context)->stop_on_main_thread();
    }

    void stop() {
        // Nothing to cancel: never wait for the main thread for nothing.
        if (scope == nullptr)
            return;
        if (pthread_main_np() != 0)
            stop_on_main_thread();
        else
            dispatch_sync_f(dispatch_get_main_queue(), this, &Impl::stop_thunk);
    }

    bool start(int interval_ms, std::function<void()> fn) {
        stop();
        scope = std::make_unique<platform::MainThreadScope>();
        timer = std::make_unique<platform::MainThreadTimer>(*scope, std::move(fn));
        timer->fire_every(std::chrono::milliseconds(interval_ms));
        return true;
    }

    bool running() const {
        return scope != nullptr;
    }
};

#else

struct MainThreadTimer::Impl {
    struct Chain {
        bool running = true;
        int interval_ms = 0;
        std::function<void()> tick;
    };
    std::shared_ptr<Chain> chain;

    static void schedule(const std::shared_ptr<Chain>& chain) {
        events::MainThreadDispatcher::call_async_after(
            [chain] {
                if (!chain->running)
                    return;
                chain->tick();
                if (chain->running)
                    schedule(chain);
            },
            chain->interval_ms);
    }

    void stop() {
        if (chain) {
            chain->running = false;
            chain->tick = nullptr;
            chain.reset();
        }
    }

    bool start(int interval_ms, std::function<void()> fn) {
        stop();
        chain = std::make_shared<Chain>();
        chain->interval_ms = interval_ms;
        chain->tick = std::move(fn);
        schedule(chain);
        return true;
    }

    bool running() const {
        return chain != nullptr;
    }
};

#endif

MainThreadTimer::MainThreadTimer() : impl_(std::make_unique<Impl>()) {}

MainThreadTimer::~MainThreadTimer() {
    impl_->stop();
}

bool MainThreadTimer::start(int interval_ms, std::function<void()> tick) {
    return impl_->start(interval_ms, std::move(tick));
}

void MainThreadTimer::stop() {
    impl_->stop();
}

bool MainThreadTimer::running() const {
    return impl_->running();
}

bool is_main_thread() {
#if defined(__APPLE__)
    return pthread_main_np() != 0;
#else
    return events::MainThreadDispatcher::is_main_thread();
#endif
}

bool post_to_main_thread(std::function<void()>& fn) {
#if defined(__APPLE__)
    // One process-wide scope: posts run in the common modes, so they are not
    // held back while a main-queue block runs a nested loop. Its run-loop
    // source is invalidated when the binary's statics are destroyed.
    static platform::MainThreadScope scope;
    if (!fn || !scope.handle().alive())
        return false;
    scope.post(std::move(fn));
    fn = nullptr;
    return true;
#else
    // The posted task and this frame share one box; the task empties it after
    // running, so whichever side lets go last frees only an empty function.
    auto box = std::make_shared<std::function<void()>>(std::move(fn));
    fn = nullptr;
    if (events::MainThreadDispatcher::call_async([box] {
            auto task = std::move(*box);
            *box = nullptr;
            task();
        }))
        return true;
    fn = std::move(*box);
    return false;
#endif
}

} // namespace pulp::format::detail
