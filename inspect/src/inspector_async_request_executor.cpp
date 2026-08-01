#include "inspector_async_request_executor.hpp"

#include <algorithm>
#include <utility>

namespace pulp::inspect::detail {

std::shared_ptr<InspectorAsyncRequestExecutor>
InspectorAsyncRequestExecutor::create(Callbacks callbacks) {
    auto executor = std::shared_ptr<InspectorAsyncRequestExecutor>(
        new InspectorAsyncRequestExecutor(std::move(callbacks)));
    executor->start();
    return executor;
}

void InspectorAsyncRequestExecutor::start() {
    if (callbacks_.reserve_worker)
        callbacks_.reserve_worker();
    try {
        const auto owner = shared_from_this();
        worker_ = std::thread([owner] { owner->run(); });
    } catch (...) {
        if (callbacks_.cancel_worker_reservation)
            callbacks_.cancel_worker_reservation();
        throw;
    }
}

void InspectorAsyncRequestExecutor::configure(std::size_t max_requests) {
    std::lock_guard lock(mutex_);
    max_requests_ = std::clamp<std::size_t>(max_requests, 1, 256);
}

bool InspectorAsyncRequestExecutor::submit(Request request) {
    {
        std::lock_guard lock(mutex_);
        const auto admitted = queue_.size() + (request_active_ ? 1u : 0u);
        if (stopping_ || admitted >= max_requests_)
            return false;
        queue_.push_back(std::move(request));
    }
    cv_.notify_one();
    return true;
}

bool InspectorAsyncRequestExecutor::cancel_queued_for_client(
    const std::shared_ptr<InspectorConnectedClient>& client,
    std::function<void()> deferred_cleanup) {
    if (!client)
        return false;
    std::lock_guard lock(mutex_);
    std::erase_if(queue_, [&](const auto& work) {
        return work.client == client;
    });
    if (active_client_ != client)
        return false;
    deferred_cleanups_.insert_or_assign(
        client.get(), std::move(deferred_cleanup));
    return true;
}

void InspectorAsyncRequestExecutor::clear() {
    std::lock_guard lock(mutex_);
    queue_.clear();
}

void InspectorAsyncRequestExecutor::stop(bool detach_worker) {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    bool expected = false;
    if (!stop_claimed_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }
    if (!worker_.joinable())
        return;
    if (detach_worker || on_worker_thread())
        worker_.detach();
    else
        worker_.join();
}

bool InspectorAsyncRequestExecutor::on_worker_thread() const noexcept {
    return worker_.joinable() && worker_.get_id() == std::this_thread::get_id();
}

void InspectorAsyncRequestExecutor::run() {
    if (callbacks_.worker_started)
        callbacks_.worker_started();
    struct ExitGuard {
        std::function<void()> callback;
        ~ExitGuard() { if (callback) callback(); }
    } exit_guard{callbacks_.worker_exited};

    std::unique_lock lock(mutex_);
    while (true) {
        cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (stopping_)
            break;
        auto request = std::move(queue_.front());
        queue_.pop_front();
        request_active_ = true;
        active_client_ = request.client;
        lock.unlock();
        auto lifetime = callbacks_.acquire_iteration_lifetime
            ? callbacks_.acquire_iteration_lifetime()
            : std::shared_ptr<void>{};
        if (lifetime && callbacks_.execute)
            callbacks_.execute(request);
        lock.lock();
        request_active_ = false;
        active_client_.reset();
        std::function<void()> deferred_cleanup;
        if (const auto found = deferred_cleanups_.find(request.client.get());
            found != deferred_cleanups_.end()) {
            deferred_cleanup = std::move(found->second);
            deferred_cleanups_.erase(found);
        }
        cv_.notify_all();
        lock.unlock();
        if (deferred_cleanup)
            deferred_cleanup();
        // Releasing the server callback may destroy the server and stop this
        // executor from the worker thread. Do that while the queue mutex is
        // unlocked so reentrant teardown can mark the executor stopping.
        lifetime.reset();
        lock.lock();
    }
}

} // namespace pulp::inspect::detail
