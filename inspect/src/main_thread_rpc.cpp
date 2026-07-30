#include <pulp/inspect/main_thread_rpc.hpp>

#include <pulp/events/main_thread_dispatcher.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pulp::inspect {
namespace {

InspectorMessage cancelled(std::int64_t request_id) {
    return make_error(request_id,
                      "Inspector dispatch was cancelled during teardown",
                      "dispatch_cancelled");
}

InspectorMessage run_operation(std::int64_t request_id,
                               const InspectorMainThreadRpc::Operation& operation) {
    try {
        return operation();
    } catch (const std::exception& error) {
        return make_error(request_id,
                          std::string("Inspector dispatch failed: ") + error.what(),
                          "dispatch_failed");
    } catch (...) {
        return make_error(request_id,
                          "Inspector dispatch failed",
                          "dispatch_failed");
    }
}

struct PendingCall {
    explicit PendingCall(std::int64_t id) : request_id(id) {}

    std::int64_t request_id = 0;
    std::mutex mutex;
    std::condition_variable cv;
    bool started = false;
    bool completed = false;
    bool slot_held = true;
    InspectorMessage response;
};

} // namespace

class InspectorMainThreadRpc::Impl {
public:
    Config config;
    Post post;
    IsMainThread is_main_thread;
    std::atomic<bool> accepting{true};
    std::recursive_mutex operation_mutex;
    std::mutex pending_mutex;
    std::vector<std::weak_ptr<PendingCall>> pending;
    std::size_t pending_count = 0;

    bool register_call(const std::shared_ptr<PendingCall>& call) {
        std::lock_guard lock(pending_mutex);
        pending.erase(
            std::remove_if(pending.begin(), pending.end(),
                           [](const auto& item) { return item.expired(); }),
            pending.end());
        if (!accepting.load(std::memory_order_acquire) ||
            pending_count >= config.max_pending)
            return false;
        pending.emplace_back(call);
        ++pending_count;
        return true;
    }

    void complete_call(const std::shared_ptr<PendingCall>& call,
                       InspectorMessage response) {
        bool notify = false;
        bool release_slot = false;
        {
            std::lock_guard lock(call->mutex);
            if (!call->completed) {
                call->response = std::move(response);
                call->completed = true;
                notify = true;
            }
            release_slot = call->slot_held;
            call->slot_held = false;
        }
        if (notify)
            call->cv.notify_all();

        if (!release_slot)
            return;
        std::lock_guard lock(pending_mutex);
        if (pending_count > 0)
            --pending_count;
        pending.erase(
            std::remove_if(pending.begin(), pending.end(),
                           [](const auto& item) { return item.expired(); }),
            pending.end());
    }
};

InspectorMainThreadRpc::InspectorMainThreadRpc()
    : InspectorMainThreadRpc(Config{}) {}

InspectorMainThreadRpc::InspectorMainThreadRpc(Config config)
    : InspectorMainThreadRpc(
          config,
          [](std::function<void()> task) {
              return pulp::events::MainThreadDispatcher::call_async(
                  std::move(task));
          },
          [] { return pulp::events::MainThreadDispatcher::is_main_thread(); }) {}

InspectorMainThreadRpc::InspectorMainThreadRpc(
    Config config,
    Post post,
    IsMainThread is_main_thread)
    : impl_(std::make_shared<Impl>()) {
    impl_->config = config;
    if (impl_->config.timeout <= std::chrono::milliseconds(0))
        impl_->config.timeout = std::chrono::milliseconds(1);
    if (impl_->config.max_pending == 0)
        impl_->config.max_pending = 1;
    impl_->post = std::move(post);
    impl_->is_main_thread = std::move(is_main_thread);
}

InspectorMainThreadRpc::~InspectorMainThreadRpc() {
    cancel();
}

InspectorMessage InspectorMainThreadRpc::call(
    std::int64_t request_id,
    Operation operation) {
    if (!operation) {
        return make_error(request_id,
                          "Inspector dispatch operation is empty",
                          "invalid_dispatch");
    }
    if (impl_->is_main_thread && impl_->is_main_thread()) {
        std::lock_guard operation_lock(impl_->operation_mutex);
        if (!impl_->accepting.load(std::memory_order_acquire))
            return cancelled(request_id);
        return run_operation(request_id, operation);
    }
    if (!impl_->accepting.load(std::memory_order_acquire))
        return cancelled(request_id);

    auto pending = std::make_shared<PendingCall>(request_id);
    if (!impl_->register_call(pending)) {
        if (!impl_->accepting.load(std::memory_order_acquire))
            return cancelled(request_id);
        return make_error(request_id,
                          "Inspector main-thread queue is full",
                          "dispatch_queue_full");
    }

    const auto impl = impl_;
    const bool posted = impl_->post && impl_->post(
        [impl, pending, request_id, operation = std::move(operation)]() mutable {
            std::lock_guard operation_lock(impl->operation_mutex);
            {
                std::lock_guard lock(pending->mutex);
                if (pending->completed)
                    return;
            }
            if (!impl->accepting.load(std::memory_order_acquire)) {
                impl->complete_call(pending, cancelled(request_id));
                return;
            }
            {
                std::lock_guard lock(pending->mutex);
                if (pending->completed)
                    return;
                pending->started = true;
            }
            impl->complete_call(
                pending, run_operation(request_id, operation));
        });
    if (!posted) {
        impl_->complete_call(
            pending,
            make_error(request_id,
                       "No main-thread dispatcher accepted the request",
                       "main_thread_unavailable"));
    }

    std::unique_lock lock(pending->mutex);
    if (!pending->cv.wait_for(lock, impl_->config.timeout,
                              [&] { return pending->completed; })) {
        const bool may_have_applied = pending->started;
        pending->completed = true;
        pending->slot_held = false;
        lock.unlock();
        {
            std::lock_guard pending_lock(impl_->pending_mutex);
            if (impl_->pending_count > 0)
                --impl_->pending_count;
        }

        auto data = std::string(R"({"mayHaveApplied":)") +
                    (may_have_applied ? "true}" : "false}");
        return make_error(request_id,
                          "Inspector main-thread dispatch timed out",
                          "main_thread_timeout",
                          std::move(data));
    }
    return pending->response;
}

void InspectorMainThreadRpc::cancel() {
    if (!impl_)
        return;
    const bool was_accepting =
        impl_->accepting.exchange(false, std::memory_order_acq_rel);
    std::lock_guard operation_lock(impl_->operation_mutex);
    if (!was_accepting)
        return;

    std::vector<std::shared_ptr<PendingCall>> calls;
    {
        std::lock_guard lock(impl_->pending_mutex);
        for (const auto& item : impl_->pending) {
            if (auto call = item.lock())
                calls.push_back(std::move(call));
        }
    }
    for (const auto& call : calls) {
        bool started = false;
        {
            std::lock_guard lock(call->mutex);
            started = call->started;
        }
        if (!started)
            impl_->complete_call(call, cancelled(call->request_id));
    }
}

bool InspectorMainThreadRpc::active() const {
    return impl_ && impl_->accepting.load(std::memory_order_acquire);
}

} // namespace pulp::inspect
