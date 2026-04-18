#include <pulp/events/timer.hpp>

namespace pulp::events {

Timer::Timer(EventLoop& loop, Duration interval, Callback callback, bool repeating)
    : loop_(loop)
    , interval_(interval)
    , callback_(std::move(callback))
    , repeating_(repeating)
    , alive_(std::make_shared<std::atomic<bool>>(true))
{
}

Timer::~Timer() {
    stop();
}

void Timer::start() {
    // Fresh sentinel for this lifecycle. Safe to reassign alive_ here
    // because start() runs on the owner thread — no in-flight dispatch
    // lambda can be copying alive_ concurrently at this point; any
    // prior-cycle lambdas hold their own shared_ptr copies captured
    // before stop() flipped them to false.
    //
    // Previously this reassignment lived in stop(), racing with
    // schedule_next()'s `auto alive = alive_;` copy on the event-loop
    // thread — std::shared_ptr is NOT thread-safe for concurrent
    // read/write of the same instance. TSan caught it as a
    // std::swap-of-atomic<bool>* race in test_events.cpp "Timer basic
    // operation". Issue #414.
    alive_ = std::make_shared<std::atomic<bool>>(true);
    active_.store(true, std::memory_order_release);
    schedule_next();
}

void Timer::stop() {
    active_.store(false, std::memory_order_release);
    alive_->store(false, std::memory_order_release);
    // Don't replace alive_ here. In-flight dispatch lambdas still
    // hold a shared_ptr copy to this sentinel and correctly see the
    // false value set above; the old sentinel is reclaimed when the
    // last in-flight lambda drops its ref. start() allocates a fresh
    // sentinel for the next lifecycle.
}

void Timer::set_interval(Duration interval) {
    interval_ = interval;
}

void Timer::schedule_next() {
    auto alive = alive_;
    auto* self = this;
    loop_.dispatch_after(interval_, [self, alive]() {
        if (!alive->load(std::memory_order_acquire))
            return;
        if (!self->active_.load(std::memory_order_acquire))
            return;
        self->callback_();
        if (self->repeating_ && self->active_.load(std::memory_order_acquire)) {
            self->schedule_next();
        }
    });
}

} // namespace pulp::events
