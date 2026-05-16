#include <pulp/events/event_loop.hpp>
#include <algorithm>

namespace pulp::events {

EventLoop::EventLoop() {
    thread_ = std::thread([this] {
        thread_id_ = std::this_thread::get_id();
        run();
    });
}

EventLoop::~EventLoop() {
    stop();
}

void EventLoop::dispatch(Task task) {
    {
        std::lock_guard lock(mutex_);
        pending_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void EventLoop::dispatch_after(Duration delay, Task task) {
    auto when = Clock::now() + delay;
    {
        std::lock_guard lock(mutex_);
        timed_.push_back({when, std::move(task)});
    }
    cv_.notify_one();
}

bool EventLoop::is_current_thread() const {
    return std::this_thread::get_id() == thread_id_;
}

void EventLoop::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel))
        return;
    cv_.notify_one();
    if (thread_.joinable())
        thread_.join();
}

void EventLoop::run() {
    while (running_.load(std::memory_order_acquire)) {
        std::vector<Task> tasks;
        {
            std::unique_lock lock(mutex_);

            // Find the earliest timed task deadline
            auto deadline = TimePoint::max();
            for (const auto& t : timed_) {
                if (t.when < deadline) deadline = t.when;
            }

            // Wait for work or deadline
            if (pending_.empty() && (timed_.empty() || Clock::now() < deadline)) {
                if (timed_.empty()) {
                    cv_.wait(lock, [this] {
                        return !pending_.empty() || !timed_.empty() ||
                               !running_.load(std::memory_order_acquire);
                    });
                } else {
                    cv_.wait_until(lock, deadline);
                }
            }

            if (!running_.load(std::memory_order_acquire))
                break;

            // Collect ready immediate tasks
            tasks = std::move(pending_);
            pending_.clear();

            // Collect ready timed tasks
            auto now = Clock::now();
            auto it = std::partition(timed_.begin(), timed_.end(),
                [now](const TimedTask& t) { return t.when > now; });
            for (auto ready = it; ready != timed_.end(); ++ready) {
                tasks.push_back(std::move(ready->task));
            }
            timed_.erase(it, timed_.end());
        }

        // Execute outside the lock
        for (auto& task : tasks) {
            if (task) task();
        }
    }
}

} // namespace pulp::events
