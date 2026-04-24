#include <pulp/events/timer.hpp>

namespace pulp::events {

struct Timer::Impl {
    EventLoop& loop;
    Duration interval;
    Callback callback;
    bool repeating;
    std::atomic<bool> active{false};
    // Cycle generation — incremented by stop() so that stale dispatch
    // lambdas scheduled before stop() return early when they fire. Cheap
    // replacement for the previous shared_ptr<atomic<bool>> sentinel
    // which raced on the shared_ptr slot between start() (main thread)
    // and schedule_next() (event-loop thread). See #687 / #414.
    std::atomic<std::uint64_t> generation{0};

    Impl(EventLoop& l, Duration i, Callback cb, bool rep)
        : loop(l), interval(i), callback(std::move(cb)), repeating(rep) {}
};

Timer::Timer(EventLoop& loop, Duration interval, Callback callback, bool repeating)
    : impl_(std::make_shared<Impl>(loop, interval, std::move(callback), repeating))
{
}

Timer::~Timer() {
    // Flip active + bump generation so any already-queued dispatch lambda
    // short-circuits. The lambdas hold their own shared_ptr<Impl> copies,
    // so Impl stays alive until they finish even though *our* impl_ is
    // about to drop its reference — no UAF. See #716.
    stop();
}

void Timer::start() {
    // Idempotent when already running. See #438 P1 Codex review on #428.
    if (impl_->active.load(std::memory_order_acquire)) {
        return;
    }
    impl_->active.store(true, std::memory_order_release);
    schedule_next(impl_, impl_->generation.load(std::memory_order_acquire));
}

void Timer::stop() {
    impl_->active.store(false, std::memory_order_release);
    // Atomic RMW — safe to race with start() and schedule_next().
    impl_->generation.fetch_add(1, std::memory_order_acq_rel);
}

bool Timer::is_active() const {
    return impl_->active.load(std::memory_order_acquire);
}

void Timer::set_interval(Duration interval) {
    impl_->interval = interval;
}

Duration Timer::interval() const {
    return impl_->interval;
}

void Timer::schedule_next(std::shared_ptr<Impl> impl, std::uint64_t gen) {
    auto& loop = impl->loop;
    auto interval = impl->interval;
    // Capture the shared_ptr by value. If the owning Timer is destroyed
    // before this lambda fires, `impl` keeps the state alive until we
    // finish — the atomics return "inactive + generation moved on" and
    // we exit without touching freed memory. #716 fix.
    loop.dispatch_after(interval, [impl, gen]() {
        if (!impl->active.load(std::memory_order_acquire))
            return;
        if (impl->generation.load(std::memory_order_acquire) != gen)
            return;
        impl->callback();
        if (!impl->repeating)
            return;
        if (!impl->active.load(std::memory_order_acquire))
            return;
        if (impl->generation.load(std::memory_order_acquire) != gen)
            return;
        schedule_next(impl, gen);
    });
}

} // namespace pulp::events
