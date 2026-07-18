#include <pulp/playback/compile_executor.hpp>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace pulp::playback {

bool DeferredCompileExecutor::submit(std::unique_ptr<CompileTask> task,
                                     std::chrono::steady_clock::time_point not_before) {
    if (!task) return false;
    tasks_.push_back({not_before, std::move(task)});
    return true;
}

std::size_t DeferredCompileExecutor::run_for(std::chrono::microseconds budget,
                                             std::size_t max_work_units) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    std::size_t slices = 0;
    for (std::size_t index = 0; index < tasks_.size() &&
         std::chrono::steady_clock::now() < deadline;) {
        if (tasks_[index].not_before > std::chrono::steady_clock::now()) {
            ++index;
            continue;
        }
        auto task = std::move(tasks_[index].task);
        tasks_.erase(tasks_.begin() + static_cast<std::ptrdiff_t>(index));
        ++slices;
        if (task->run_slice({deadline, max_work_units}) == CompileTaskStatus::Pending)
            tasks_.push_back({std::chrono::steady_clock::now(), std::move(task)});
    }
    return slices;
}

struct WorkerCompileExecutor::Impl {
#if !defined(PULP_COMPILE_EXECUTOR_DISABLE_THREADS)
    struct Entry {
        std::chrono::steady_clock::time_point not_before;
        std::unique_ptr<CompileTask> task;
    };
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<Entry> tasks;
    bool stopping = false;
    std::thread worker;

    Impl() : worker([this] { run(); }) {}
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        worker.join();
    }
    void run() {
        std::unique_lock lock(mutex);
        for (;;) {
            if (stopping) return;
            if (tasks.empty()) {
                wake.wait(lock, [this] { return stopping || !tasks.empty(); });
                continue;
            }
            const auto next = std::min_element(tasks.begin(), tasks.end(),
                [](const Entry& a, const Entry& b) { return a.not_before < b.not_before; });
            if (next->not_before > std::chrono::steady_clock::now()) {
                // A notification may mean a newly submitted task has an earlier
                // deadline. Always recompute the minimum after waking.
                wake.wait_until(lock, next->not_before);
                continue;
            }
            auto task = std::move(next->task);
            tasks.erase(next);
            lock.unlock();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
            const auto status = task->run_slice({deadline, 256});
            lock.lock();
            if (status == CompileTaskStatus::Pending)
                tasks.push_back({std::chrono::steady_clock::now(), std::move(task)});
        }
    }
#endif
};

WorkerCompileExecutor::WorkerCompileExecutor() : impl_(std::make_unique<Impl>()) {}
WorkerCompileExecutor::~WorkerCompileExecutor() = default;

bool WorkerCompileExecutor::submit(std::unique_ptr<CompileTask> task,
                                   std::chrono::steady_clock::time_point not_before) {
#if defined(PULP_COMPILE_EXECUTOR_DISABLE_THREADS)
    (void)task;
    (void)not_before;
    return false;
#else
    if (!task) return false;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) return false;
        impl_->tasks.push_back({not_before, std::move(task)});
    }
    impl_->wake.notify_one();
    return true;
#endif
}

bool WorkerCompileExecutor::supported() const noexcept {
#if defined(PULP_COMPILE_EXECUTOR_DISABLE_THREADS)
    return false;
#else
    return true;
#endif
}

} // namespace pulp::playback
