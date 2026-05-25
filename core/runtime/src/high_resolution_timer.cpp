#include <pulp/runtime/high_resolution_timer.hpp>

#ifdef __APPLE__
#include <mach/mach_time.h>
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
    stop();
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
    }

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
