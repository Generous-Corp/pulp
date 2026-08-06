#include <pulp/inspect/main_thread_rpc.hpp>

#include <pulp/events/main_thread_dispatcher.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pulp::inspect {
namespace {

InspectorMessage cancelled(std::int64_t request_id) {
    return make_error(request_id, "Inspector dispatch was cancelled during teardown",
                      "dispatch_cancelled");
}

InspectorMessage run_operation(std::int64_t request_id,
                               const InspectorMainThreadRpc::Operation& operation) {
    try {
        return operation();
    } catch (const std::exception& error) {
        return make_error(request_id, std::string("Inspector dispatch failed: ") + error.what(),
                          "dispatch_failed");
    } catch (...) {
        return make_error(request_id, "Inspector dispatch failed", "dispatch_failed");
    }
}

void run_completion(InspectorMainThreadRpc::Completion completion) noexcept {
    if (!completion)
        return;
    try {
        completion();
    } catch (...) {
    }
}

struct PendingCall {
    PendingCall(std::int64_t id, InspectorMainThreadRpc::Operation pending_operation,
                InspectorMainThreadRpc::Completion done)
        : request_id(id), operation(std::move(pending_operation)), completion(std::move(done)) {}

    std::int64_t request_id = 0;
    std::mutex mutex;
    std::condition_variable cv;
    bool started = false;
    bool response_ready = false;
    bool cancelled_before_start = false;
    bool slot_held = true;
    bool completion_called = false;
    InspectorMessage response;
    InspectorMainThreadRpc::Operation operation;
    InspectorMainThreadRpc::Completion completion;
};

} // namespace

class InspectorMainThreadRpc::Impl {
  public:
    struct PostedLifetimeCallbacks {
        InspectorMainThreadRpc::Completion begin;
        InspectorMainThreadRpc::Completion end;
    };

    Config config;
    Post post;
    IsMainThread is_main_thread;
    std::atomic<bool> accepting{true};
    std::recursive_mutex operation_mutex;
    std::mutex pending_mutex;
    std::condition_variable pending_cv;
    std::vector<std::weak_ptr<PendingCall>> pending;
    std::size_t pending_count = 0;
    std::size_t posted_lifetime_count = 0;
    std::shared_ptr<const PostedLifetimeCallbacks> posted_lifetime_callbacks;
    std::map<std::thread::id, std::size_t> operation_threads;
    std::map<std::thread::id, std::vector<InspectorMainThreadRpc::Completion>> after_operation;

    void begin_operation() {
        std::lock_guard lock(pending_mutex);
        ++operation_threads[std::this_thread::get_id()];
    }

    void end_operation() {
        std::vector<InspectorMainThreadRpc::Completion> completions;
        {
            std::lock_guard lock(pending_mutex);
            const auto thread = std::this_thread::get_id();
            if (auto found = operation_threads.find(thread);
                found != operation_threads.end() && --found->second == 0) {
                operation_threads.erase(found);
                if (auto callbacks = after_operation.find(thread);
                    callbacks != after_operation.end()) {
                    completions = std::move(callbacks->second);
                    after_operation.erase(callbacks);
                }
            }
        }
        pending_cv.notify_all();
        for (auto& completion : completions)
            run_completion(std::move(completion));
    }

    bool executing_here() {
        std::lock_guard lock(pending_mutex);
        return operation_threads.contains(std::this_thread::get_id());
    }

    bool defer_here(InspectorMainThreadRpc::Completion completion) {
        if (!completion)
            return false;
        std::lock_guard lock(pending_mutex);
        const auto thread = std::this_thread::get_id();
        if (!operation_threads.contains(thread))
            return false;
        after_operation[thread].push_back(std::move(completion));
        return true;
    }

    class OperationGuard {
      public:
        explicit OperationGuard(std::shared_ptr<Impl> owner) : impl(std::move(owner)) {
            impl->begin_operation();
        }
        ~OperationGuard() {
            impl->end_operation();
        }

      private:
        std::shared_ptr<Impl> impl;
    };

    class PostedLifetime {
      public:
        explicit PostedLifetime(std::shared_ptr<Impl> owner) : impl(std::move(owner)) {
            {
                std::lock_guard lock(impl->pending_mutex);
                ++impl->posted_lifetime_count;
                callbacks = impl->posted_lifetime_callbacks;
            }
            try {
                if (callbacks && callbacks->begin)
                    callbacks->begin();
            } catch (...) {
            }
        }

        ~PostedLifetime() {
            {
                std::lock_guard lock(impl->pending_mutex);
                --impl->posted_lifetime_count;
            }
            impl->pending_cv.notify_all();
            try {
                if (callbacks && callbacks->end)
                    callbacks->end();
            } catch (...) {
            }
        }

      private:
        std::shared_ptr<Impl> impl;
        std::shared_ptr<const PostedLifetimeCallbacks> callbacks;
    };

    bool register_call(const std::shared_ptr<PendingCall>& call) {
        std::lock_guard lock(pending_mutex);
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [](const auto& item) { return item.expired(); }),
                      pending.end());
        if (!accepting.load(std::memory_order_acquire) || pending_count >= config.max_pending)
            return false;
        pending.emplace_back(call);
        ++pending_count;
        return true;
    }

    void complete_call(const std::shared_ptr<PendingCall>& call, InspectorMessage response) {
        bool notify = false;
        bool release_slot = false;
        InspectorMainThreadRpc::Operation retired_operation;
        InspectorMainThreadRpc::Completion completion;
        {
            std::lock_guard lock(call->mutex);
            if (!call->response_ready) {
                call->response = std::move(response);
                call->response_ready = true;
                notify = true;
            }
            retired_operation = std::move(call->operation);
            release_slot = call->slot_held;
            call->slot_held = false;
            if (!call->completion_called) {
                call->completion_called = true;
                completion = std::move(call->completion);
            }
        }
        retired_operation = {};
        if (notify)
            call->cv.notify_all();
        run_completion(std::move(completion));

        if (!release_slot)
            return;
        std::lock_guard lock(pending_mutex);
        if (pending_count > 0)
            --pending_count;
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [](const auto& item) { return item.expired(); }),
                      pending.end());
        pending_cv.notify_all();
    }

    void fail_post_admission(const std::shared_ptr<PendingCall>& call, std::int64_t request_id,
                             std::string message, std::string code) {
        InspectorMainThreadRpc::Operation retired_operation;
        bool complete_before_start = false;
        bool notify_started_failure = false;
        {
            std::lock_guard lock(call->mutex);
            if (call->response_ready)
                return;
            const bool may_have_applied = call->started;
            call->response = make_error(request_id, std::move(message), std::move(code),
                                        std::string(R"({"mayHaveApplied":)") +
                                            (may_have_applied ? "true}" : "false}"));
            call->response_ready = true;
            if (call->started) {
                notify_started_failure = true;
            } else {
                call->cancelled_before_start = true;
                retired_operation = std::move(call->operation);
                complete_before_start = true;
            }
        }
        retired_operation = {};
        if (notify_started_failure) {
            call->cv.notify_all();
            return;
        }
        if (complete_before_start) {
            complete_call(call, {});
            call->cv.notify_all();
        }
    }
};

InspectorMainThreadRpc::InspectorMainThreadRpc() : InspectorMainThreadRpc(Config{}) {}

InspectorMainThreadRpc::InspectorMainThreadRpc(Config config)
    : InspectorMainThreadRpc(
          config,
          [](std::function<void()> task) {
              return pulp::events::MainThreadDispatcher::call_async(std::move(task));
          },
          [] { return pulp::events::MainThreadDispatcher::is_main_thread(); }) {}

InspectorMainThreadRpc::InspectorMainThreadRpc(Config config, Post post,
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

InspectorMessage InspectorMainThreadRpc::call(std::int64_t request_id, Operation operation) {
    return call(request_id, std::move(operation), {});
}

std::chrono::milliseconds InspectorMainThreadRpc::default_timeout() const {
    return impl_->config.timeout;
}

InspectorMessage InspectorMainThreadRpc::call(std::int64_t request_id, Operation operation,
                                              Completion completion) {
    return call(request_id, std::move(operation), std::move(completion), impl_->config.timeout);
}

InspectorMessage InspectorMainThreadRpc::call(std::int64_t request_id, Operation operation,
                                              Completion completion,
                                              std::chrono::milliseconds timeout) {
    return call_with_inline_policy(request_id, std::move(operation), std::move(completion), timeout,
                                   true);
}

InspectorMessage InspectorMainThreadRpc::call_queued_only(std::int64_t request_id,
                                                          Operation operation,
                                                          Completion completion,
                                                          std::chrono::milliseconds timeout) {
    return call_with_inline_policy(request_id, std::move(operation), std::move(completion), timeout,
                                   false);
}

InspectorMessage InspectorMainThreadRpc::call_with_inline_policy(std::int64_t request_id,
                                                                 Operation operation,
                                                                 Completion completion,
                                                                 std::chrono::milliseconds timeout,
                                                                 bool allow_inline) {
    const auto impl = impl_;
    if (timeout <= std::chrono::milliseconds::zero())
        timeout = std::chrono::milliseconds(1);
    if (!operation) {
        run_completion(std::move(completion));
        return make_error(request_id, "Inspector dispatch operation is empty", "invalid_dispatch");
    }
    if (impl->is_main_thread && impl->is_main_thread()) {
        if (!allow_inline) {
            run_completion(std::move(completion));
            return make_error(request_id,
                              "Queued-only Inspector dispatch cannot originate on the main thread",
                              "direct_main_thread_forbidden", R"({"mayHaveApplied":false})");
        }
        const auto started_at = std::chrono::steady_clock::now();
        std::unique_lock operation_lock(impl->operation_mutex);
        if (!impl->accepting.load(std::memory_order_acquire)) {
            run_completion(std::move(completion));
            return cancelled(request_id);
        }
        if (std::chrono::steady_clock::now() - started_at >= timeout) {
            run_completion(std::move(completion));
            operation_lock.unlock();
            return make_error(request_id, "Inspector main-thread dispatch timed out",
                              "main_thread_timeout", R"({"mayHaveApplied":false})");
        }
        Impl::OperationGuard running(impl);
        auto response = run_operation(request_id, operation);
        run_completion(std::move(completion));
        const bool timed_out = std::chrono::steady_clock::now() - started_at >= timeout;
        // after_current_operation callbacks may drain transports whose reader
        // threads are waiting to enter this serialized RPC. Release the
        // serialization mutex before OperationGuard runs those callbacks.
        operation_lock.unlock();
        if (timed_out) {
            return make_error(request_id, "Inspector main-thread dispatch timed out",
                              "main_thread_timeout", R"({"mayHaveApplied":true})");
        }
        return response;
    }
    if (!impl->accepting.load(std::memory_order_acquire)) {
        run_completion(std::move(completion));
        return cancelled(request_id);
    }

    auto pending =
        std::make_shared<PendingCall>(request_id, std::move(operation), std::move(completion));
    if (!impl->register_call(pending)) {
        auto response = !impl->accepting.load(std::memory_order_acquire)
                            ? cancelled(request_id)
                            : make_error(request_id, "Inspector main-thread queue is full",
                                         "dispatch_queue_full");
        {
            std::lock_guard lock(pending->mutex);
            pending->slot_held = false;
        }
        impl->complete_call(pending, response);
        return response;
    }

    std::function<void()> posted_task = [lifetime = std::make_shared<Impl::PostedLifetime>(impl),
                                         impl, pending, request_id]() mutable {
        (void)lifetime;
        std::unique_lock operation_lock(impl->operation_mutex);
        bool should_run = false;
        bool cancelled_before_start = false;
        Operation operation;
        {
            std::lock_guard lock(pending->mutex);
            cancelled_before_start = pending->cancelled_before_start;
            if (cancelled_before_start) {
                operation = std::move(pending->operation);
            } else if (impl->accepting.load(std::memory_order_acquire)) {
                pending->started = true;
                operation = std::move(pending->operation);
                should_run = true;
            } else {
                pending->cancelled_before_start = true;
                operation = std::move(pending->operation);
            }
        }
        if (cancelled_before_start) {
            operation = {};
            return;
        }
        if (!should_run) {
            operation = {};
            impl->complete_call(pending, cancelled(request_id));
            return;
        }
        Impl::OperationGuard running(impl);
        auto response = run_operation(request_id, operation);
        impl->complete_call(pending, std::move(response));
        // See the direct path above: deferred teardown must not wait for a
        // reader that is itself blocked on this mutex.
        operation_lock.unlock();
    };
    bool posted = false;
    bool post_threw = false;
    try {
        if (impl->post)
            posted = impl->post(std::move(posted_task));
    } catch (...) {
        post_threw = true;
    }
    posted_task = {};
    if (post_threw) {
        impl->fail_post_admission(pending, request_id,
                                  "Main-thread dispatcher admission threw an exception",
                                  "dispatch_failed");
    } else if (!posted) {
        impl->fail_post_admission(pending, request_id,
                                  "No main-thread dispatcher accepted the request",
                                  "main_thread_unavailable");
    }

    std::unique_lock lock(pending->mutex);
    if (!pending->cv.wait_for(lock, timeout, [&] { return pending->response_ready; })) {
        const bool may_have_applied = pending->started;
        auto timeout_response = make_error(
            request_id, "Inspector main-thread dispatch timed out", "main_thread_timeout",
            std::string(R"({"mayHaveApplied":)") + (may_have_applied ? "true}" : "false}"));
        if (may_have_applied) {
            // The posted task owns the pending state and operation capture.
            // Fence this caller at the deadline; complete_call() will release
            // the slot and discard the late response when execution returns.
            pending->response_ready = true;
        } else {
            pending->cancelled_before_start = true;
        }
        Operation retired_operation;
        if (!may_have_applied)
            retired_operation = std::move(pending->operation);
        lock.unlock();
        retired_operation = {};
        if (!may_have_applied)
            impl->complete_call(pending, timeout_response);
        return timeout_response;
    }
    return pending->response;
}

void InspectorMainThreadRpc::cancel() {
    const auto impl = impl_;
    if (!impl)
        return;
    const bool was_accepting = impl->accepting.exchange(false, std::memory_order_acq_rel);
    if (!was_accepting)
        return;

    std::vector<std::shared_ptr<PendingCall>> calls;
    {
        std::lock_guard lock(impl->pending_mutex);
        for (const auto& item : impl->pending) {
            if (auto call = item.lock())
                calls.push_back(std::move(call));
        }
    }
    for (const auto& call : calls) {
        bool cancel_before_start = false;
        Operation retired_operation;
        {
            std::lock_guard lock(call->mutex);
            if (!call->started && !call->response_ready) {
                call->cancelled_before_start = true;
                retired_operation = std::move(call->operation);
                cancel_before_start = true;
            }
        }
        retired_operation = {};
        if (cancel_before_start)
            impl->complete_call(call, cancelled(call->request_id));
    }
}

void InspectorMainThreadRpc::cancel_and_wait() {
    cancel();
    const auto impl = impl_;
    if (!impl || impl->executing_here())
        return;
    std::unique_lock lock(impl->pending_mutex);
    impl->pending_cv.wait(
        lock, [&] { return impl->pending_count == 0 && impl->operation_threads.empty(); });
}

bool InspectorMainThreadRpc::set_posted_lifetime_callbacks(Completion begin, Completion end) {
    const auto impl = impl_;
    if (!impl)
        return false;
    auto callbacks = std::make_shared<Impl::PostedLifetimeCallbacks>();
    callbacks->begin = std::move(begin);
    callbacks->end = std::move(end);
    std::lock_guard lock(impl->pending_mutex);
    if (impl->posted_lifetime_count != 0 || impl->posted_lifetime_callbacks)
        return false;
    impl->posted_lifetime_callbacks = std::move(callbacks);
    return true;
}

bool InspectorMainThreadRpc::executing_on_current_thread() const {
    return impl_ && impl_->executing_here();
}

bool InspectorMainThreadRpc::after_current_operation(Completion completion) {
    return impl_ && impl_->defer_here(std::move(completion));
}

bool InspectorMainThreadRpc::active() const {
    return impl_ && impl_->accepting.load(std::memory_order_acquire);
}

} // namespace pulp::inspect
