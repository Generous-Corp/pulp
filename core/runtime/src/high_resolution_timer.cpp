#include <pulp/runtime/high_resolution_timer.hpp>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

namespace pulp::runtime {

HighResolutionTimer::~HighResolutionTimer() {
    stop();
}

void HighResolutionTimer::start(std::chrono::microseconds interval,
                                std::function<void()> callback) {
    stop();
    callback_ = std::move(callback);
    running_.store(true, std::memory_order_release);

    thread_ = std::thread([this, interval]() {
        // Set thread priority high for timing accuracy
#ifdef __APPLE__
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#elif defined(__linux__)
        struct sched_param param;
        param.sched_priority = sched_get_priority_max(SCHED_FIFO);
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
#endif

        auto next = std::chrono::steady_clock::now() + interval;

        while (running_.load(std::memory_order_acquire)) {
            if (callback_)
                callback_();

            // Busy-wait for short intervals (<1ms), sleep for longer
            if (interval < std::chrono::milliseconds(1)) {
                while (std::chrono::steady_clock::now() < next &&
                       running_.load(std::memory_order_relaxed)) {
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
    if (thread_.joinable())
        thread_.join();
}

}  // namespace pulp::runtime
