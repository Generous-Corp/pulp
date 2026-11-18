#include <pulp/format/detail/main_thread_timer.hpp>

#include "host_run_loop_lease.hpp"

#include <pulp/events/main_thread_dispatcher.hpp>
#include <pulp/platform/main_thread.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

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

// Off Apple the main thread is either a host's, reached through the run loop an
// adapter lent to the platform layer (HostRunLoopLease), or a standalone's,
// reached through MainThreadDispatcher. A host loop is preferred when one is
// registered and the caller is on its UI thread.
struct MainThreadTimer::Impl {
    // The dispatcher fallback: a chain of one-shot posts.
    struct Chain {
        bool running = true;
        int interval_ms = 0;
        std::function<void()> tick;
    };
    std::shared_ptr<Chain> chain;

    // The host-loop timer: a platform timer in a platform scope. `live` is read
    // by the tick, so a stop from any thread ends ticking at once; the objects
    // themselves are destroyed on the UI thread.
    struct Hosted {
        std::shared_ptr<std::atomic<bool>> live;
        std::unique_ptr<platform::MainThreadScope> scope;
        std::unique_ptr<platform::MainThreadTimer> timer;
    };
    std::shared_ptr<Hosted> hosted;

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
        if (platform::is_main_thread())
            drain_deferred();
        if (chain) {
            chain->running = false;
            chain->tick = nullptr;
            chain.reset();
        }
        if (hosted) {
            *hosted->live = false;
            auto owned = std::move(hosted);
            if (platform::is_main_thread()) {
                owned->timer.reset();
                owned->scope.reset();
            } else {
                // Not the UI thread: the host's loop may only be touched
                // there, so the UI thread does the teardown.
                auto handle = owned->scope->handle();
                const bool posted = handle.post([owned]() mutable {
                    owned->timer.reset();
                    owned->scope.reset();
                });
                if (!posted)
                    defer_teardown(std::move(owned));
            }
        }
    }

    // The loop was withdrawn before the UI thread could tear this down. Keep
    // the state, untouched, until a later call on the UI thread destroys it:
    // nothing is destroyed on the caller's thread.
    static std::mutex& deferred_mutex() {
        static std::mutex* m = new std::mutex;
        return *m;
    }
    static std::vector<std::shared_ptr<Hosted>>& deferred() {
        static auto* v = new std::vector<std::shared_ptr<Hosted>>;
        return *v;
    }
    static void defer_teardown(std::shared_ptr<Hosted> owned) {
        std::lock_guard<std::mutex> lock(deferred_mutex());
        deferred().push_back(std::move(owned));
    }
    static void drain_deferred() {
        std::vector<std::shared_ptr<Hosted>> take;
        {
            std::lock_guard<std::mutex> lock(deferred_mutex());
            take.swap(deferred());
        }
        for (auto& h : take) {
            h->timer.reset();
            h->scope.reset();
        }
    }

    bool start(int interval_ms, std::function<void()> fn) {
        stop();
        if (platform::is_main_thread())
            drain_deferred();
        if (platform::main_thread_supported() && platform::is_main_thread()) {
            auto state = std::make_shared<Hosted>();
            state->live = std::make_shared<std::atomic<bool>>(true);
            state->scope = std::make_unique<platform::MainThreadScope>();
            auto live = state->live;
            state->timer = std::make_unique<platform::MainThreadTimer>(*state->scope, [live, fn] {
                if (live->load())
                    fn();
            });
            state->timer->fire_every(std::chrono::milliseconds(interval_ms));
            if (state->timer->pending()) {
                hosted = std::move(state);
                return true;
            }
            // The host refused the timer: fall back to the dispatcher chain.
            *state->live = false;
        }
        chain = std::make_shared<Chain>();
        chain->interval_ms = interval_ms;
        chain->tick = std::move(fn);
        schedule(chain);
        return true;
    }

    bool running() const {
        return chain != nullptr || hosted != nullptr;
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
    return platform::main_thread_supported() ? platform::is_main_thread()
                                             : events::MainThreadDispatcher::is_main_thread();
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
    // A standalone's main thread is reached through MainThreadDispatcher; an
    // editor inside a host has no such backend, and its adapter's lease
    // reaches the host's run loop instead.
    if (!events::MainThreadDispatcher::has_backend())
        return post_through_host_run_loop(fn);
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
