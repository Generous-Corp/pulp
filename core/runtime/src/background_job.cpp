#include <pulp/runtime/background_job.hpp>

#include <algorithm>

namespace pulp::runtime {

BackgroundJobContext::BackgroundJobContext(CancellationToken cancellation,
                                           std::function<void(BackgroundJobProgress)> progress)
    : cancellation_(std::move(cancellation))
    , progress_(std::move(progress)) {}

bool BackgroundJobContext::is_cancelled() const {
    return cancellation_.is_cancelled();
}

void BackgroundJobContext::publish_progress(BackgroundJobProgress progress) const {
    if (progress_) progress_(std::move(progress));
}

BackgroundJobHandle::BackgroundJobHandle(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

void BackgroundJobHandle::cancel() const {
    if (auto state = state_) {
        state->token.cancel();
    }
}

bool BackgroundJobHandle::valid() const {
    return static_cast<bool>(state_);
}

bool BackgroundJobHandle::is_cancelled() const {
    if (auto state = state_) {
        return state->token.is_cancelled();
    }
    return false;
}

bool BackgroundJobHandle::is_finished() const {
    if (auto state = state_) {
        std::lock_guard lock(state->mutex);
        return state->finished;
    }
    return true;
}

void BackgroundJobHandle::wait() const {
    if (auto state = state_) {
        std::unique_lock lock(state->mutex);
        state->cv.wait(lock, [&] { return state->finished; });
    }
}

std::optional<BackgroundJobProgress> BackgroundJobHandle::latest_progress() const {
    if (auto state = state_) {
        std::lock_guard lock(state->mutex);
        return state->progress;
    }
    return std::nullopt;
}

BackgroundJobService::BackgroundJobService()
    : worker_([this] { worker_loop(); }) {}

BackgroundJobService::~BackgroundJobService() {
    cancel_all();
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

BackgroundJobHandle BackgroundJobService::submit(BackgroundJobOptions options,
                                                 BackgroundJobFunction job) {
    static std::atomic<std::uint64_t> next_sequence{0};
    auto state = std::make_shared<BackgroundJobHandle::State>(CancellationToken{});
    BackgroundJobHandle handle(state);

    {
        std::lock_guard lock(mutex_);
        prune_finished_jobs_locked();
        queue_.push_back(QueuedJob{
            .options = std::move(options),
            .job = std::move(job),
            .state = state,
            .sequence = next_sequence.fetch_add(1, std::memory_order_relaxed),
        });
        all_jobs_.push_back(state);
        std::stable_sort(queue_.begin(), queue_.end(), has_higher_priority);
    }

    cv_.notify_one();
    return handle;
}

void BackgroundJobService::cancel_all() {
    std::vector<std::shared_ptr<BackgroundJobHandle::State>> jobs;
    {
        std::lock_guard lock(mutex_);
        jobs = all_jobs_;
    }

    for (auto& job : jobs) {
        if (job) job->token.cancel();
    }
}

void BackgroundJobService::wait_all() {
    std::vector<std::shared_ptr<BackgroundJobHandle::State>> jobs;
    {
        std::lock_guard lock(mutex_);
        jobs = all_jobs_;
    }

    for (auto& job : jobs) {
        if (!job) continue;
        std::unique_lock lock(job->mutex);
        job->cv.wait(lock, [&] { return job->finished; });
    }

    {
        std::lock_guard lock(mutex_);
        prune_finished_jobs_locked();
    }
}

std::size_t BackgroundJobService::pending_count() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

void BackgroundJobService::worker_loop() {
    for (;;) {
        QueuedJob item;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) return;

            item = std::move(queue_.front());
            queue_.pop_front();
        }

        {
            std::lock_guard lock(item.state->mutex);
            item.state->started = true;
        }

        auto publish_progress = [state = item.state](BackgroundJobProgress progress) {
            std::lock_guard lock(state->mutex);
            state->progress = std::move(progress);
        };

        BackgroundJobContext context(item.state->token, std::move(publish_progress));
        if (item.job && !context.is_cancelled()) {
            item.job(context);
        }

        {
            std::lock_guard lock(item.state->mutex);
            item.state->finished = true;
        }
        item.state->cv.notify_all();

        {
            std::lock_guard lock(mutex_);
            prune_finished_jobs_locked();
        }
    }
}

void BackgroundJobService::prune_finished_jobs_locked() {
    all_jobs_.erase(
        std::remove_if(all_jobs_.begin(), all_jobs_.end(),
                       [](const std::shared_ptr<BackgroundJobHandle::State>& state) {
                           if (!state) return true;
                           std::lock_guard lock(state->mutex);
                           return state->finished;
                       }),
        all_jobs_.end());
}

bool BackgroundJobService::has_higher_priority(const QueuedJob& a, const QueuedJob& b) {
    if (a.options.priority != b.options.priority) {
        return static_cast<int>(a.options.priority) > static_cast<int>(b.options.priority);
    }
    return a.sequence < b.sequence;
}

} // namespace pulp::runtime
