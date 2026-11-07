#include <pulp/runtime/cancel.hpp>

#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace pulp::runtime {

static_assert(std::atomic<bool>::is_always_lock_free,
              "CancelToken::is_cancelled() must be lock-free to be audio-thread safe");

// Shared state for one source and all of its tokens. `cancelled` is read
// lock-free; everything else is guarded by `mutex`.
struct CancelState final : detail::CancelStateBase {
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::uint64_t, CancelCallback> callbacks; // ordered by registration
    std::uint64_t next_id = 1;
    std::uint64_t running_id = 0; // callback currently executing, 0 if none
    std::thread::id running_thread;
};

namespace {
CancelState* as_state(const std::shared_ptr<detail::CancelStateBase>& base) noexcept {
    return static_cast<CancelState*>(base.get()); // only CancelState is ever stored
}
} // namespace

std::shared_ptr<CancelToken::State> CancelToken::make_state() {
    return std::make_shared<CancelState>();
}

bool CancelToken::request_cancel(const std::shared_ptr<State>& base) noexcept {
    if (!base)
        return false;
    CancelState* state = as_state(base);
    std::unique_lock<std::mutex> lock(state->mutex);
    if (state->cancelled.load(std::memory_order_relaxed))
        return false;
    state->cancelled.store(true, std::memory_order_release);
    state->cv.notify_all();

    // Drain callbacks one at a time so a registration destroyed concurrently
    // can either remove its entry or, if already running, wait for it.
    while (!state->callbacks.empty()) {
        auto it = state->callbacks.begin();
        const std::uint64_t id = it->first;
        CancelCallback fn = std::move(it->second);
        state->callbacks.erase(it);
        state->running_id = id;
        state->running_thread = std::this_thread::get_id();
        lock.unlock();
        try {
            if (fn)
                fn();
        } catch (...) {
        }
        fn = nullptr; // destroy captures outside the lock, before waiters wake
        lock.lock();
        state->running_id = 0;
        state->running_thread = std::thread::id{};
        state->cv.notify_all();
    }
    return true;
}

bool CancelToken::wait_for_cancellation(std::chrono::nanoseconds timeout) const {
    if (timeout < std::chrono::nanoseconds::zero())
        timeout = std::chrono::nanoseconds::zero();
    // Clamp so that adding to steady_clock::now() cannot overflow.
    constexpr auto kMax = std::chrono::hours(24 * 365);
    if (timeout > kMax)
        timeout = kMax;

    if (!state_) {
        if (timeout > std::chrono::nanoseconds::zero())
            std::this_thread::sleep_for(timeout);
        return false;
    }
    CancelState* state = as_state(state_);
    std::unique_lock<std::mutex> lock(state->mutex);
    return state->cv.wait_for(lock, timeout,
                              [state] { return state->cancelled.load(std::memory_order_acquire); });
}

CancelRegistration CancelToken::register_callback(CancelCallback fn) const {
    if (!state_ || !fn)
        return {};
    {
        CancelState* state = as_state(state_);
        std::unique_lock<std::mutex> lock(state->mutex);
        if (!state->cancelled.load(std::memory_order_relaxed)) {
            const std::uint64_t id = state->next_id++;
            state->callbacks.emplace(id, std::move(fn));
            return CancelRegistration(state_, id);
        }
    }
    try {
        fn();
    } catch (...) {
    }
    return {};
}

CancelRegistration::~CancelRegistration() {
    reset();
}

CancelRegistration::CancelRegistration(CancelRegistration&& other) noexcept
    : state_(std::move(other.state_)), id_(std::exchange(other.id_, 0)) {}

CancelRegistration& CancelRegistration::operator=(CancelRegistration&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

void CancelRegistration::reset() noexcept {
    if (!state_ || id_ == 0) {
        state_.reset();
        id_ = 0;
        return;
    }
    {
        CancelState* state = as_state(state_);
        std::unique_lock<std::mutex> lock(state->mutex);
        if (state->callbacks.erase(id_) == 0 && state->running_id == id_ &&
            state->running_thread != std::this_thread::get_id()) {
            // Mid-callback on another thread: wait so the callback's captures
            // are not used after this registration's owner has gone.
            const std::uint64_t id = id_;
            state->cv.wait(lock, [state, id] { return state->running_id != id; });
        }
    }
    state_.reset();
    id_ = 0;
}

CancelSource::CancelSource() : token_(CancelToken::make_state()) {}

CancelSource::~CancelSource() {
    cancel();
}

CancelSource& CancelSource::operator=(CancelSource&& other) noexcept {
    if (this != &other) {
        cancel();
        token_ = std::exchange(other.token_, CancelToken{});
    }
    return *this;
}

bool CancelSource::cancel() noexcept {
    return CancelToken::request_cancel(token_.state_);
}

CancellationToken::CancellationToken() : CancelToken(make_state()) {}

} // namespace pulp::runtime
