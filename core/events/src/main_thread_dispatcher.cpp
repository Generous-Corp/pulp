#include <pulp/events/main_thread_dispatcher.hpp>
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pulp::events {
namespace {

struct DispatcherState {
    struct Registration {
        MainThreadDispatcher::Token token;
        MainThreadDispatcher::Backend backend;
        bool active = true;
        std::size_t in_flight = 0;
    };

    std::mutex mutex;
    std::condition_variable cv;
    MainThreadDispatcher::Token next_token = 1;
    std::vector<std::shared_ptr<Registration>> registrations;
};

DispatcherState& state() {
    static DispatcherState s;
    return s;
}

constexpr std::size_t kMaxCallSyncBackendAttempts = 64;

thread_local std::vector<MainThreadDispatcher::Token> active_callback_tokens;

std::size_t active_backend_callback_depth(MainThreadDispatcher::Token token) {
    return static_cast<std::size_t>(
        std::count(active_callback_tokens.begin(), active_callback_tokens.end(), token));
}

class BackendCallbackScope {
public:
    explicit BackendCallbackScope(MainThreadDispatcher::Token token)
        : token_(token) {
        if (token_ != 0)
            active_callback_tokens.push_back(token_);
    }

    BackendCallbackScope(const BackendCallbackScope&) = delete;
    BackendCallbackScope& operator=(const BackendCallbackScope&) = delete;

    ~BackendCallbackScope() {
        if (token_ != 0)
            active_callback_tokens.pop_back();
    }

private:
    MainThreadDispatcher::Token token_;
};

class BackendLease {
public:
    BackendLease() = default;
    explicit BackendLease(std::shared_ptr<DispatcherState::Registration> registration)
        : registration_(std::move(registration)) {}

    BackendLease(const BackendLease&) = delete;
    BackendLease& operator=(const BackendLease&) = delete;

    BackendLease(BackendLease&& other) noexcept
        : registration_(std::move(other.registration_)) {}

    BackendLease& operator=(BackendLease&& other) noexcept {
        if (this != &other) {
            release();
            registration_ = std::move(other.registration_);
        }
        return *this;
    }

    ~BackendLease() {
        release();
    }

    MainThreadDispatcher::Backend* backend() {
        return registration_ ? &registration_->backend : nullptr;
    }

    MainThreadDispatcher::Token token() const {
        return registration_ ? registration_->token : 0;
    }

    bool active() const {
        if (!registration_)
            return false;

        auto& s = state();
        std::lock_guard lock(s.mutex);
        return registration_->active;
    }

    void release() {
        if (!registration_)
            return;

        auto registration = std::move(registration_);
        auto& s = state();
        {
            std::lock_guard lock(s.mutex);
            if (registration->in_flight > 0)
                --registration->in_flight;
        }
        s.cv.notify_all();
    }

private:
    std::shared_ptr<DispatcherState::Registration> registration_;
};

BackendLease acquire_current_backend() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.registrations.empty())
        return {};

    auto registration = s.registrations.back();
    if (!registration->active)
        return {};

    ++registration->in_flight;
    return BackendLease(std::move(registration));
}

Task make_noexcept_task(Task task) {
    return [task = std::move(task)]() mutable noexcept {
        try {
            if (task)
                task();
        } catch (...) {
        }
    };
}

} // namespace

MainThreadDispatcher::Token MainThreadDispatcher::register_backend(Backend backend) {
    if (!backend.post || !backend.is_main_thread)
        return 0;

    auto& s = state();
    std::lock_guard lock(s.mutex);

    auto token = s.next_token++;
    if (s.next_token == 0)
        s.next_token = 1;

    auto registration = std::make_shared<DispatcherState::Registration>();
    registration->token = token;
    registration->backend = std::move(backend);
    s.registrations.push_back(std::move(registration));
    return token;
}

bool MainThreadDispatcher::unregister_backend(Token token) {
    if (token == 0)
        return false;

    auto& s = state();
    std::unique_lock lock(s.mutex);
    auto it = std::find_if(s.registrations.begin(), s.registrations.end(),
        [token](const auto& registration) {
            return registration->token == token;
        });
    if (it == s.registrations.end())
        return false;

    auto registration = *it;
    registration->active = false;
    s.registrations.erase(it);
    auto self_depth = active_backend_callback_depth(token);
    s.cv.wait(lock, [&] { return registration->in_flight <= self_depth; });
    return true;
}

bool MainThreadDispatcher::has_backend() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return !s.registrations.empty();
}

bool MainThreadDispatcher::is_main_thread() {
    auto lease = acquire_current_backend();
    auto* backend = lease.backend();
    if (!backend || !backend->is_main_thread)
        return false;

    BackendCallbackScope scope(lease.token());
    return backend->is_main_thread();
}

bool MainThreadDispatcher::call_async(Task task) {
    if (!task)
        return false;

    auto lease = acquire_current_backend();
    auto* backend = lease.backend();
    if (!backend || !backend->post)
        return false;

    BackendCallbackScope scope(lease.token());
    return backend->post(make_noexcept_task(std::move(task)));
}

bool MainThreadDispatcher::call_async_after(Task task, int delay_ms) {
    if (!task)
        return false;

    auto lease = acquire_current_backend();
    auto* backend = lease.backend();
    if (!backend || !backend->post)
        return false;

    BackendCallbackScope scope(lease.token());
    auto wrapped = make_noexcept_task(std::move(task));
    if (backend->post_after && delay_ms > 0)
        return backend->post_after(std::move(wrapped), delay_ms);
    return backend->post(std::move(wrapped));
}

bool MainThreadDispatcher::call_sync(Task task) {
    if (!task)
        return false;

    for (std::size_t attempt = 0; attempt < kMaxCallSyncBackendAttempts; ++attempt) {
        auto lease = acquire_current_backend();
        auto* backend = lease.backend();
        if (!backend || !backend->post || !backend->is_main_thread)
            return false;

        auto on_main_thread = false;
        {
            BackendCallbackScope scope(lease.token());
            on_main_thread = backend->is_main_thread();
        }

        if (!lease.active())
            continue;

        if (on_main_thread) {
            lease.release();
            task();
            return true;
        }

        struct SyncState {
            std::mutex mutex;
            std::condition_variable cv;
            bool done = false;
            std::exception_ptr exception;
        };

        auto sync = std::make_shared<SyncState>();
        auto task_ptr = std::make_shared<Task>(std::move(task));

        auto posted = false;
        {
            BackendCallbackScope scope(lease.token());
            posted = backend->post([sync, task_ptr] {
                std::exception_ptr caught;
                try {
                    if (*task_ptr)
                        (*task_ptr)();
                } catch (...) {
                    caught = std::current_exception();
                }

                {
                    std::lock_guard lock(sync->mutex);
                    sync->exception = caught;
                    sync->done = true;
                }
                sync->cv.notify_one();
            });
        }
        lease.release();

        if (!posted)
            return false;

        std::unique_lock lock(sync->mutex);
        sync->cv.wait(lock, [&] { return sync->done; });
        if (sync->exception)
            std::rethrow_exception(sync->exception);
        return true;
    }

    throw std::runtime_error("MainThreadDispatcher::call_sync backend changed while dispatching");
}

} // namespace pulp::events
