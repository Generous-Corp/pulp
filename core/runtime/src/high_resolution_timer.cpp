#include <pulp/runtime/high_resolution_timer.hpp>

#ifdef __APPLE__
#include <mach/mach_time.h>
#include <dispatch/dispatch.h>
#elif defined(__linux__)
#include <sched.h>
#include <pthread.h>
#endif

namespace pulp::runtime {

struct HighResolutionTimer::TimerState {
    std::atomic<bool> running{false};
    std::atomic<bool> thread_ready{false};
    std::function<void()> callback;
};

HighResolutionTimer::~HighResolutionTimer() {
    stop();
}

void HighResolutionTimer::start(std::chrono::microseconds interval,
                                std::function<void()> callback) {
    start(interval, std::move(callback), TimerMode::DedicatedThread);
}

void HighResolutionTimer::start(std::chrono::microseconds interval,
                                std::function<void()> callback,
                                TimerMode mode) {
    stop();

#ifdef __APPLE__
    if (mode == TimerMode::OsTimerQueue) {
        // Pin the callback on the shared TimerState so the dispatch-source
        // block can keep it alive through the timer's lifetime.
        auto state = std::make_shared<TimerState>();
        state->callback = std::move(callback);
        state->running.store(true, std::memory_order_release);

        dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
        dispatch_queue_t queue =
            dispatch_queue_create("com.pulp.runtime.HighResolutionTimer", attr);
        dispatch_source_t source = dispatch_source_create(
            DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
        // Release ownership of the queue to the source (which keeps it alive).
        dispatch_release(queue);

        const uint64_t interval_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(interval).count());
        dispatch_source_set_timer(source,
                                  dispatch_time(DISPATCH_TIME_NOW, interval_ns),
                                  interval_ns,
                                  /* leeway */ 0);

        std::shared_ptr<TimerState> state_for_block = state;
        dispatch_source_set_event_handler(source, ^{
            if (state_for_block->running.load(std::memory_order_acquire)
                && state_for_block->callback) {
                state_for_block->callback();
            }
        });

        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = state;
            dispatch_source_ = source;
            active_mode_ = TimerMode::OsTimerQueue;
            running_.store(true, std::memory_order_release);
        }

        dispatch_resume(source);
        return;
    }
#else
    (void) mode; // OsTimerQueue falls through to DedicatedThread off-Apple.
#endif

    active_mode_ = TimerMode::DedicatedThread;
    auto state = std::make_shared<TimerState>();
    state->callback = std::move(callback);
    state->running.store(true, std::memory_order_release);

    std::thread thread([state, interval]() {
        state->thread_ready.wait(false, std::memory_order_acquire);

        // Set thread priority high for timing accuracy
#ifdef __APPLE__
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#elif defined(__linux__)
        // Best-effort priority boost — may fail without CAP_SYS_NICE
        struct sched_param param{};
        param.sched_priority = sched_get_priority_max(SCHED_FIFO);
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);  // Ignored if no permission
#endif

        auto next = std::chrono::steady_clock::now() + interval;

        while (state->running.load(std::memory_order_acquire)) {
            if (state->callback)
                state->callback();

            if (!state->running.load(std::memory_order_acquire))
                break;

            // Busy-wait for short intervals (<1ms), sleep for longer
            if (interval < std::chrono::milliseconds(1)) {
                while (std::chrono::steady_clock::now() < next &&
                       state->running.load(std::memory_order_relaxed)) {
                    // Spin
                }
            } else {
                std::this_thread::sleep_until(next);
            }
            next += interval;
        }
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = state;
        thread_ = std::move(thread);
        running_.store(true, std::memory_order_release);
    }

    state->thread_ready.store(true, std::memory_order_release);
    state->thread_ready.notify_one();
}

void HighResolutionTimer::stop() {
    std::shared_ptr<TimerState> state;
    std::thread thread;
    bool stop_from_timer_thread = false;
    void* source_to_release = nullptr;

    running_.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = std::move(state_);
        if (state) {
            state->running.store(false, std::memory_order_release);
            state->thread_ready.store(true, std::memory_order_release);
            state->thread_ready.notify_one();
        }

        if (thread_.joinable()) {
            stop_from_timer_thread = thread_.get_id() == std::this_thread::get_id();
            thread = std::move(thread_);
        }

        source_to_release = dispatch_source_;
        dispatch_source_ = nullptr;
        active_mode_ = TimerMode::DedicatedThread;
    }

#ifdef __APPLE__
    if (source_to_release) {
        dispatch_source_t source = static_cast<dispatch_source_t>(source_to_release);
        dispatch_source_cancel(source);
        dispatch_release(source);
    }
#else
    (void) source_to_release;
#endif

    if (stop_from_timer_thread) {
        // A callback can stop or destroy its timer; hand the join to another
        // thread so std::thread's destructor never sees a self-owned handle.
        std::thread reaper([thread = std::move(thread)]() mutable {
            if (thread.joinable())
                thread.join();
        });
        reaper.detach();
    } else if (thread.joinable()) {
        thread.join();
    }
}

}  // namespace pulp::runtime
