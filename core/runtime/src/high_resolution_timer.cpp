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
    state_ = state;
    running_.store(true, std::memory_order_release);

    thread_ = std::thread([state, interval]() {
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
}

void HighResolutionTimer::stop() {
    running_.store(false, std::memory_order_release);
    if (state_)
        state_->running.store(false, std::memory_order_release);

    if (thread_.joinable() && thread_.get_id() == std::this_thread::get_id()) {
        // A callback can stop or destroy its timer; hand the join to another
        // thread so std::thread's destructor never sees a self-owned handle.
        std::thread reaper([thread = std::move(thread_)]() mutable {
            if (thread.joinable())
                thread.join();
        });
        reaper.detach();
    } else if (thread_.joinable()) {
        thread_.join();
    }
    state_.reset();
}

}  // namespace pulp::runtime
