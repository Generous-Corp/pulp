#include <pulp/events/timer.hpp>

namespace pulp::events {

Timer::Timer(EventLoop& loop, Duration interval, Callback callback, bool repeating)
    : loop_(loop)
    , interval_(interval)
    , callback_(std::move(callback))
    , repeating_(repeating)
{
}

Timer::~Timer() {
    stop();
}

void Timer::start() {
    // Idempotent when already running. See #438 P1 Codex review on #428.
    if (active_.load(std::memory_order_acquire)) {
        return;
    }
    active_.store(true, std::memory_order_release);
    schedule_next(generation_.load(std::memory_order_acquire));
}

void Timer::stop() {
    active_.store(false, std::memory_order_release);
    // Bump the generation so any in-flight dispatch lambda whose captured
    // generation no longer matches returns early before calling the
    // callback or rescheduling. Atomic RMW — safe to race with start()
    // and schedule_next().
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

void Timer::set_interval(Duration interval) {
    interval_ = interval;
}

void Timer::schedule_next(std::uint64_t gen) {
    auto* self = this;
    loop_.dispatch_after(interval_, [self, gen]() {
        if (!self->active_.load(std::memory_order_acquire))
            return;
        if (self->generation_.load(std::memory_order_acquire) != gen)
            return;
        self->callback_();
        if (!self->repeating_)
            return;
        if (!self->active_.load(std::memory_order_acquire))
            return;
        if (self->generation_.load(std::memory_order_acquire) != gen)
            return;
        self->schedule_next(gen);
    });
}

} // namespace pulp::events
