#include <pulp/format/detail/main_thread_timer.hpp>

#include <pulp/events/main_thread_dispatcher.hpp>

#include <cstdint>
#include <utility>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <pthread.h>
#endif

namespace pulp::format::detail {

#if defined(__APPLE__)

struct MainThreadTimer::Impl {
    dispatch_source_t source = nullptr;
    std::function<void()> tick;

    static void fire(void* context) {
        auto* self = static_cast<Impl*>(context);
        if (self->tick)
            self->tick();
    }

    // Cancel, on the main thread where the handler runs, so no tick is in
    // progress while cancelling and none is invoked afterwards.
    void stop_on_main_thread() {
        if (source == nullptr)
            return;
        dispatch_source_cancel(source);
        dispatch_release(source);
        source = nullptr;
        tick = nullptr;
    }

    static void stop_thunk(void* context) {
        static_cast<Impl*>(context)->stop_on_main_thread();
    }

    void stop() {
        if (pthread_main_np() != 0)
            stop_on_main_thread();
        else
            dispatch_sync_f(dispatch_get_main_queue(), this, &Impl::stop_thunk);
    }

    bool start(int interval_ms, std::function<void()> fn) {
        stop();
        source =
            dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        if (source == nullptr)
            return false;
        tick = std::move(fn);
        const auto interval = static_cast<std::uint64_t>(interval_ms) * NSEC_PER_MSEC;
        dispatch_set_context(source, this);
        dispatch_source_set_event_handler_f(source, &Impl::fire);
        dispatch_source_set_timer(source, dispatch_time(DISPATCH_TIME_NOW, interval), interval,
                                  interval / 10);
        dispatch_resume(source);
        return true;
    }

    bool running() const {
        return source != nullptr;
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

} // namespace pulp::format::detail
